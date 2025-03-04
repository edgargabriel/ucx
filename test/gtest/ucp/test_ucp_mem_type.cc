/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2017. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "ucp_test.h"
#include <common/mem_buffer.h>

extern "C" {
#include <uct/api/uct.h>
#include <ucp/core/ucp_context.h>
#include <ucp/core/ucp_mm.h>
}


class test_ucp_mem_type : public ucp_test {
public:
    static void get_test_variants_base(std::vector<ucp_test_variant>& variants) {
        add_variant(variants, UCP_FEATURE_TAG);
    }

    static void get_test_variants(std::vector<ucp_test_variant>& variants) {
        add_variant_memtypes(variants, get_test_variants_base);
    }

protected:
    ucs_memory_type_t mem_type() const {
        return static_cast<ucs_memory_type_t>(get_variant_value());
    }
};

UCS_TEST_P(test_ucp_mem_type, detect) {

    const size_t size                      = 256;
    const ucs_memory_type_t alloc_mem_type = mem_type();
    ucp_memory_info_t mem_info;

    mem_buffer b(size, alloc_mem_type);

    ucp_memory_detect(sender().ucph(), b.ptr(), size, &mem_info);
    EXPECT_EQ(alloc_mem_type, mem_info.type);
}

UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_mem_type, all, "all")

class test_ucp_mem_type_alloc_before_init : public test_ucp_mem_type {
public:
    test_ucp_mem_type_alloc_before_init() {
        m_size = 10000;
    }

    virtual void init() {
        m_send_buffer.reset(new mem_buffer(m_size, mem_type()));
        m_recv_buffer.reset(new mem_buffer(m_size, mem_type()));
        test_ucp_mem_type::init();
    }

    virtual void cleanup() {
        test_ucp_mem_type::cleanup();
        m_send_buffer.reset();
        m_recv_buffer.reset();
    }

    static const uint64_t SEED = 0x1111111111111111lu;
protected:
    size_t                     m_size;
    ucs::auto_ptr<mem_buffer>  m_send_buffer, m_recv_buffer;
};

UCS_TEST_P(test_ucp_mem_type_alloc_before_init, xfer) {
    sender().connect(&receiver(), get_ep_params());

    ucp_memory_info_t mem_info;
    ucp_memory_detect(sender().ucph(), m_send_buffer->ptr(), m_size, &mem_info);
    EXPECT_EQ(mem_type(), mem_info.type) << "send buffer";
    ucp_memory_detect(receiver().ucph(), m_recv_buffer->ptr(), m_size,
                      &mem_info);
    EXPECT_EQ(mem_type(), mem_info.type) << "receive buffer";

    mem_buffer::pattern_fill(m_send_buffer->ptr(), m_size, SEED, mem_type());

    for (int i = 0; i < 3; ++i) {
        mem_buffer::pattern_fill(m_recv_buffer->ptr(), m_size, 0, mem_type());

        void *sreq = ucp_tag_send_nb(sender().ep(), m_send_buffer->ptr(), m_size,
                                     ucp_dt_make_contig(1), 1,
                                     (ucp_send_callback_t)ucs_empty_function);
        void *rreq = ucp_tag_recv_nb(receiver().worker(), m_recv_buffer->ptr(),
                                     m_size, ucp_dt_make_contig(1), 1, 1,
                                     (ucp_tag_recv_callback_t)ucs_empty_function);
        request_wait(sreq);
        request_wait(rreq);

        mem_buffer::pattern_check(m_recv_buffer->ptr(), m_size, SEED, mem_type());
    }
}

UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_mem_type_alloc_before_init, all, "all")

class test_ucp_cuda : public ucp_test {
public:
    static void get_test_variants(std::vector<ucp_test_variant>& variants) {
        add_variant(variants, UCP_FEATURE_AM);
    }
};

UCS_TEST_P(test_ucp_cuda, sparse_regions) {
    const ucs_memory_type_t mem_type = UCS_MEMORY_TYPE_CUDA;
    const size_t size = 4096;
    const size_t count = 5;
    ucs_status_t status;
    void *ptr[count];
    ucp_mem_h memh[count];

    /* create contiguous CUDA registrations list */
    for (int i = 0; i < count; i++) {
        ptr[i] = mem_buffer::allocate(size, mem_type);

        if ((i > 0) && (UCS_PTR_BYTE_OFFSET(ptr[i - 1], size) != ptr[i])) {
            for (int j = 0; j < i; j++) {
                mem_buffer::release(ptr[j], mem_type);
            }
            UCS_TEST_SKIP_R("failed to create contiguous CUDA registrations list");
        }
    }

    /* make CUDA registrations list sparse */
    for (int i = 0; i < count; i++) {
        if ((i & 1) == 0) {
            mem_buffer::release(ptr[i], mem_type);
        }
    }

    for (int i = 0; i < count; i++) {
        if ((i & 1) == 0) {
            continue;
        }

        ucp_mem_map_params_t params;
        params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                            UCP_MEM_MAP_PARAM_FIELD_LENGTH;
        params.address    = ptr[i];
        params.length     = size;

        status = ucp_mem_map(sender().ucph(), &params, &memh[i]);
        ASSERT_UCS_OK(status);
    }

    for (int i = 0; i < count; i++) {
        if ((i & 1) == 0) {
            continue;
        }

        void* rkey_buffer;
        size_t rkey_buffer_size;
        status = ucp_rkey_pack(sender().ucph(), memh[i], &rkey_buffer,
                               &rkey_buffer_size);
        ASSERT_UCS_OK(status);
        ucp_rkey_buffer_release(rkey_buffer);
    }

    for (int i = 0; i < count; i++) {
        if ((i & 1) == 0) {
            continue;
        }

        status = ucp_mem_unmap(sender().ucph(), memh[i]);
        ASSERT_UCS_OK(status);
    }

    for (int i = 0; i < count; i++) {
        if ((i & 1) == 1) {
            mem_buffer::release(ptr[i], mem_type);
        }
    }
}

UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_cuda, all, "all")
