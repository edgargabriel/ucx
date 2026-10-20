/**
 * Copyright (c) Advanced Micro Devices, Inc. 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include <uct/uct_test.h>
#include <hip/hip_runtime.h>
#include <hip/hip_runtime_api.h>

/* hipMemHandleTypeFabric was added in ROCm 10.x */
#ifndef hipMemHandleTypeFabric
#  define hipMemHandleTypeFabric ((hipMemAllocationHandleType)0x8)
#endif

/*
 * Allocate a VMM buffer backed by a fabric-exportable handle.
 * Returns the mapped VA, the allocation handle, and the rounded-up size.
 * Caller must release with hipMemUnmap + hipMemAddressFree + hipMemRelease.
 */
static hipError_t
alloc_vmm_fabric(size_t size, int device, void **va_out,
                 hipMemGenericAllocationHandle_t *handle_out,
                 size_t *actual_size_out)
{
    hipMemAllocationProp prop = {};
    hipMemAccessDesc access   = {};
    size_t granularity;
    size_t aligned_size;
    void *va;
    hipError_t err;

    prop.type                 = hipMemAllocationTypePinned;
    prop.location.type        = hipMemLocationTypeDevice;
    prop.location.id          = device;
    prop.requestedHandleTypes = hipMemHandleTypeFabric;

    err = hipMemGetAllocationGranularity(&granularity, &prop,
                                         hipMemAllocationGranularityMinimum);
    if (err != hipSuccess) {
        return err;
    }

    aligned_size = (size + granularity - 1) / granularity * granularity;

    err = hipMemCreate(handle_out, aligned_size, &prop, 0);
    if (err != hipSuccess) {
        return err;
    }

    err = hipMemAddressReserve(&va, aligned_size, 0, nullptr, 0);
    if (err != hipSuccess) {
        hipMemRelease(*handle_out);
        return err;
    }

    err = hipMemMap(va, aligned_size, 0, *handle_out, 0);
    if (err != hipSuccess) {
        hipMemAddressFree(va, aligned_size);
        hipMemRelease(*handle_out);
        return err;
    }

    access.location.type = hipMemLocationTypeDevice;
    access.location.id   = device;
    access.flags         = hipMemAccessFlagsProtReadWrite;
    err = hipMemSetAccess(va, aligned_size, &access, 1);
    if (err != hipSuccess) {
        hipMemUnmap(va, aligned_size);
        hipMemAddressFree(va, aligned_size);
        hipMemRelease(*handle_out);
        return err;
    }

    *va_out          = va;
    *actual_size_out = aligned_size;
    return hipSuccess;
}


class test_rocm_fabr : public uct_test {
protected:
    void init() override {
        uct_test::init();

        if (hipSetDevice(0) != hipSuccess) {
            UCS_TEST_SKIP_R("hipSetDevice failed");
        }
        if (hipGetDevice(&m_device) != hipSuccess) {
            UCS_TEST_SKIP_R("hipGetDevice failed");
        }

        m_sender   = uct_test::create_entity(0);
        m_entities.push_back(m_sender);
        m_receiver = uct_test::create_entity(0);
        m_entities.push_back(m_receiver);
        m_sender->connect(0, *m_receiver, 0);
    }

    void cleanup() override {
        uct_test::cleanup();
    }

    /*
     * Allocate a VMM buffer with fabric-handle support, register it with @a e,
     * and fill @a mem with the allocation details.
     * Skips the test if the device or ROCm version does not support fabric VMM.
     */
    void alloc_vmm(size_t size, entity *e, uct_allocated_memory_t *mem)
    {
        void *va;
        size_t actual_size;
        hipMemGenericAllocationHandle_t handle;

        if (alloc_vmm_fabric(size, m_device, &va, &handle, &actual_size)
                != hipSuccess) {
            UCS_TEST_SKIP_R("HIP VMM fabric allocation failed — "
                            "requires ROCm 10+ on a fabric-capable GPU");
        }

        ucs_status_t status = uct_md_mem_reg(e->md(), va, actual_size,
                                              UCT_MD_MEM_ACCESS_ALL,
                                              &mem->memh);
        if (status != UCS_OK) {
            hipMemUnmap(va, actual_size);
            hipMemAddressFree(va, actual_size);
            hipMemRelease(handle);
            UCS_TEST_SKIP_R("mem_reg failed — rocm_fabr requires "
                            "VMM-allocated memory");
        }

        mem->address = va;
        mem->length  = actual_size;
        mem->md      = e->md();

        /* stash handle for cleanup */
        m_handles[va] = {handle, actual_size};
    }

    void free_vmm(uct_allocated_memory_t *mem, entity *e)
    {
        uct_md_mem_dereg_params_t params;
        params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        params.memh       = mem->memh;
        uct_md_mem_dereg(e->md(), &params);

        auto it = m_handles.find(mem->address);
        if (it != m_handles.end()) {
            hipMemUnmap(mem->address, it->second.size);
            hipMemAddressFree(mem->address, it->second.size);
            hipMemRelease(it->second.handle);
            m_handles.erase(it);
        }
    }

    static const size_t TEST_SIZE = 4096;

    entity *m_sender;
    entity *m_receiver;
    int     m_device;

    struct vmm_entry {
        hipMemGenericAllocationHandle_t handle;
        size_t size;
    };
    std::map<void*, vmm_entry> m_handles;
};


/*
 * Verify that mem_reg succeeds on VMM-allocated memory and that the
 * resulting rkey can be packed and unpacked.
 */
UCS_TEST_P(test_rocm_fabr, mem_reg_vmm)
{
    uct_allocated_memory_t mem = {};
    uct_rkey_bundle_t rkey_bundle;

    alloc_vmm(TEST_SIZE, m_sender, &mem);

    m_sender->rkey_unpack(&mem, &rkey_bundle);
    m_sender->rkey_release(&rkey_bundle);

    free_vmm(&mem, m_sender);
}


/*
 * Verify that put_zcopy between two VMM-allocated buffers completes
 * successfully and the data is correct.
 */
UCS_TEST_P(test_rocm_fabr, put_zcopy)
{
    uct_allocated_memory_t send_mem = {}, recv_mem = {};
    uct_rkey_bundle_t rkey_bundle;
    ucs_status_t status;

    alloc_vmm(TEST_SIZE, m_sender, &send_mem);
    alloc_vmm(TEST_SIZE, m_receiver, &recv_mem);

    /* Fill send buffer with a pattern */
    std::vector<uint8_t> pattern(TEST_SIZE);
    for (size_t i = 0; i < TEST_SIZE; i++) {
        pattern[i] = static_cast<uint8_t>(i & 0xFF);
    }
    ASSERT_EQ(hipSuccess,
              hipMemcpy(send_mem.address, pattern.data(), TEST_SIZE,
                        hipMemcpyHostToDevice));

    /* Pack rkey for receive buffer on sender side */
    m_receiver->rkey_unpack(&recv_mem, &rkey_bundle);

    uct_iov_t iov;
    iov.buffer = send_mem.address;
    iov.length = TEST_SIZE;
    iov.memh   = send_mem.memh;
    iov.stride = 0;
    iov.count  = 1;

    do {
        status = uct_ep_put_zcopy(m_sender->ep(0), &iov, 1,
                                   (uint64_t)recv_mem.address,
                                   rkey_bundle.rkey, nullptr);
    } while (status == UCS_ERR_NO_RESOURCE);
    ASSERT_UCS_OK_OR_INPROGRESS(status);

    flush();

    std::vector<uint8_t> result(TEST_SIZE, 0);
    ASSERT_EQ(hipSuccess,
              hipMemcpy(result.data(), recv_mem.address, TEST_SIZE,
                        hipMemcpyDeviceToHost));
    EXPECT_EQ(pattern, result);

    m_receiver->rkey_release(&rkey_bundle);
    free_vmm(&send_mem, m_sender);
    free_vmm(&recv_mem, m_receiver);
}


_UCT_INSTANTIATE_TEST_CASE(test_rocm_fabr, rocm_fabr)
