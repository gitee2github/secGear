/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * CloudEnclave is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "secure_channel_enclave.h"

#include <openssl/pem.h>
#include "status.h"
#include "secgear_log.h"
#include "secgear_random.h"
#include "secure_channel_common.h"
#include "secure_channel_t.h"

#ifdef SGX_ENCLAVE
    #include "tsgxsslio.h"
    #include "sgx_thread.h"
    typedef sgx_thread_rwlock_t sc_lock_t;
#else
    #include "base64url.h"
    #include "tee_ra_api.h"
    typedef pthread_rwlock_t sc_lock_t;
#endif

static int eccurve_support[] = {
    NID_brainpoolP256r1, // brainpoolP256r1
    NID_brainpoolP320r1, // brainpoolP320r1
    NID_brainpoolP384r1, // brainpoolP384r1
    NID_brainpoolP512r1, // brainpoolP512r1
    NID_X25519,          // X25519
    NID_X448,            // X448
};

bool is_supported_curveid(int curve_id)
{
    for (size_t i = 0; i < sizeof(eccurve_support) / sizeof(int); i++) {
        if (eccurve_support[i] == curve_id) {
            return true;
        }
    }
    return false;
}

static void sc_wtlock(sc_lock_t *lock)
{
#ifdef SGX_ENCLAVE
    (void)sgx_thread_rwlock_wrlock(lock);
#else
    (void)pthread_rwlock_wrlock(lock);
#endif
}

static void sc_wtunlock(sc_lock_t *lock)
{
#ifdef SGX_ENCLAVE
    (void)sgx_thread_rwlock_wrunlock(lock);
#else
    (void)pthread_rwlock_unlock(lock);
#endif
}

static void sc_rdlock(sc_lock_t *lock)
{
#ifdef SGX_ENCLAVE
    (void)sgx_thread_rwlock_rdlock(lock);
#else
    (void)pthread_rwlock_rdlock(lock);
#endif
}

static void sc_rdunlock(sc_lock_t *lock)
{
#ifdef SGX_ENCLAVE
    (void)sgx_thread_rwlock_rdunlock(lock);
#else
    (void)pthread_rwlock_unlock(lock);
#endif
}

static void sc_init_rwlock(sc_lock_t *lock)
{
#ifdef SGX_ENCLAVE
    (void)sgx_thread_rwlock_init(lock, NULL);
#else
    (void)pthread_rwlock_init(lock, NULL);
#endif
}

static void sc_fini_rwlock(sc_lock_t *lock)
{
#ifdef SGX_ENCLAVE
    (void)sgx_thread_rwlock_destroy(lock);
#else
    (void)pthread_rwlock_destroy(lock);
#endif
}

typedef struct sec_chl_node {
    size_t session_id;
    time_t inactive_cnt;   // the inactive count of session
    sec_chl_ecdh_ctx_t *ecdh_ctx;
    struct sec_chl_node *next;
} SEC_CHL_NODE;

/* The max secure channel connection number at the same time */
#define MAX_SEL_CHL_NUM 1031
typedef struct {
    bool          is_init;
    sc_lock_t     sec_chl_list_lock;
    SEC_CHL_NODE  sec_chl_list_head;
    size_t        count;  // secure channel connection number
} SEC_CHL_MNG;

static SEC_CHL_MNG g_sec_chl_manager = {
    .is_init           = false,
};

static SEC_CHL_NODE *new_sec_chl_node()
{
    SEC_CHL_NODE *node = (SEC_CHL_NODE *)calloc(1, sizeof(SEC_CHL_NODE));
    if (node == NULL) {
        PrintInfo(PRINT_ERROR, "malloc failed\n");
        return NULL;
    }
    node->ecdh_ctx = new_local_ecdh_ctx(NID_brainpoolP256r1);
    if (node->ecdh_ctx == NULL) {
        free(node);
        PrintInfo(PRINT_ERROR, "new local ecdh ctx failed\n");
        return NULL;
    }
    int ret = gen_local_exch_buf(node->ecdh_ctx);
    if (ret != CC_SUCCESS) {
        del_ecdh_ctx(node->ecdh_ctx);
        free(node);
        PrintInfo(PRINT_ERROR, "gen local ecdh param failed\n");
        return NULL;
    }

    node->inactive_cnt = 0;

    return node;
}

static void free_sec_chl_node(SEC_CHL_NODE *node)
{
    if (node == NULL) {
        return;
    }
    if (node->ecdh_ctx != NULL) {
        del_ecdh_ctx(node->ecdh_ctx);
    }
    free(node);
}

static int get_pubkey_buffer(RSA *rsa_key, uint8_t **pubkey, size_t *pubkey_len)
{
    BIO *r_key = NULL;
    size_t key_len;
    int ret_val = CC_FAIL;
    r_key = BIO_new(BIO_s_mem());
    if (r_key == NULL) {
        PrintInfo(PRINT_ERROR, "get pubkey buffer bio new failed\n");
        goto end;
    }

    if (!PEM_write_bio_RSAPublicKey(r_key, rsa_key)) {
        PrintInfo(PRINT_ERROR, "get pubkey buffer write bio rsa pubkey failed\n");
        goto end;
    }

    key_len = BIO_ctrl_pending(r_key);
    if (key_len > *pubkey_len) {
        PrintInfo(PRINT_ERROR, "get pubkey buffer pubkey_len:%llu is not enough\n", *pubkey_len);
        goto end;
    }
    if (BIO_read(r_key, *pubkey, key_len) != (int)key_len) {
        PrintInfo(PRINT_ERROR, "get pubkey buffer bio read failed\n");
        goto end;
    }
    *pubkey_len = key_len;

    ret_val = CC_SUCCESS;
end:
    BIO_free(r_key);
    r_key = NULL;
    return ret_val;
}

static int gen_rsa_key(RSA **rsa_key)
{
    size_t modulus_bits = 3072;
    size_t pub_exponent = RSA_F4;
    RSA *r = NULL;

    BIGNUM *bne = BN_new();
    if (bne == NULL) {
        PrintInfo(PRINT_ERROR, "gen rsa key bn_new failed\n");
        return CC_FAIL;
    }
    
    for (size_t i = 0; i < sizeof(size_t) * BYTE_TO_BIT_LEN; ++i) {
        if (pub_exponent & (1UL << i)) {
            if (BN_set_bit(bne, i) == 0) {
                PrintInfo(PRINT_ERROR, "gen rsa key bn_set_bit failed\n");
                BN_free(bne);
                return CC_FAIL;
            }
        }
    }
    r = RSA_new();
    if (r == NULL) {
        PrintInfo(PRINT_ERROR, "gen rsa key rsa_new failed\n");
        BN_free(bne);
        return CC_FAIL;
    }
    if (RSA_generate_key_ex(r, modulus_bits, bne, NULL) != 1) {
        PrintInfo(PRINT_ERROR, "gen rsa key gen key failed\n");
        BN_free(bne);
        RSA_free(r);
        return CC_FAIL;
    }
    BN_free(bne);
    *rsa_key = r;
    return CC_SUCCESS;
}

static int add_to_sec_chl_list(SEC_CHL_NODE *node)
{
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);

    if (g_sec_chl_manager.count > MAX_SEL_CHL_NUM) {
        sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
        PrintInfo(PRINT_ERROR, "secure channel client num exceed the max limit:%u\n", MAX_SEL_CHL_NUM);
        return CC_ERROR_SEC_CHL_CLI_NUM_EXCEED_MAX_LIMIT;
    }
    g_sec_chl_manager.count++;

    SEC_CHL_NODE *temp = g_sec_chl_manager.sec_chl_list_head.next;
    g_sec_chl_manager.sec_chl_list_head.next = node;
    node->next = temp;

    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);

    return CC_SUCCESS;
}

int init_session(size_t *session_id)
{
    size_t random_id = 0;
    int ret = cc_enclave_generate_random(&random_id, sizeof(size_t));
    if (ret != CC_SUCCESS) {
        PrintInfo(PRINT_ERROR, "get enclave pubkey gen random failed\n");
        return CC_ERROR_SEC_CHL_GEN_RANDOM;
    }

    SEC_CHL_NODE *node = new_sec_chl_node();
    if (node == NULL) {
        PrintInfo(PRINT_ERROR, "get enclave pubkey new sec chl node failed\n");
        return CC_FAIL;
    }

    node->session_id = random_id;
    *session_id = random_id;
    ret = add_to_sec_chl_list(node);
    if (ret != CC_SUCCESS) {
        free_sec_chl_node(node);
    }
    return ret;
}

static sec_chl_ecdh_ctx_t *get_ecdh_ctx_by_session_id(size_t session_id);

static int cache_rsa_key(size_t session_id, RSA *rsa_key)
{
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    if (ecdh_ctx == NULL) {
        sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
        return CC_FAIL;
    }
    ecdh_ctx->svr_rsa_key = rsa_key;
    ecdh_ctx->signature_len = RSA_size(rsa_key);

    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
    return CC_SUCCESS;
}

int get_enclave_pubkey(size_t *session_id, uint8_t *pubkey, size_t *pubkey_len)
{
    RSA *r = NULL;
    int ret = CC_FAIL;

    if (session_id == NULL || pubkey == NULL || pubkey_len == 0) {
        PrintInfo(PRINT_ERROR, "get enclave pubkey param error\n");
        return CC_ERROR_BAD_PARAMETERS;
    }
    // 添加到g_sec_chl_manager
    ret = init_session(session_id);
    if (ret != CC_SUCCESS) {
        PrintInfo(PRINT_ERROR, "get enclave pubkey add node failed\n");
        return ret;
    }

    // generate rsa key
    ret = gen_rsa_key(&r);
    if (ret != CC_SUCCESS) {
        // node will free by sec_chl_destroy
        return ret;
    }

    ret = get_pubkey_buffer(r, &pubkey, pubkey_len);
    if (ret != CC_SUCCESS) {
        RSA_free(r);
        PrintInfo(PRINT_ERROR, "get enclave pubkey get buffer failed\n");
        // node will free by sec_chl_destroy
        return ret;
    }

    ret = cache_rsa_key(*session_id, r);
    if (ret != CC_SUCCESS) {
        RSA_free(r);
        PrintInfo(PRINT_ERROR, "get enclave pubkey cache ras key failed\n");
        // node will free by sec_chl_destroy
    }
    return ret;
}

static sec_chl_ecdh_ctx_t *get_ecdh_ctx_by_session_id(size_t session_id)
{
    SEC_CHL_NODE *p = g_sec_chl_manager.sec_chl_list_head.next;
    while (p != NULL) {
        if (p->session_id == session_id) {
            p->inactive_cnt = 0;
            break;
        }
        p = p->next;
    }
    if (p == NULL) {
        PrintInfo(PRINT_ERROR, "not found ecdh ctx by session_id:%llu\n", session_id);
        return NULL;
    }
    return p->ecdh_ctx;
}

int get_enclave_exch_param_len(size_t session_id, size_t *exch_param_len)
{
    sc_rdlock(&g_sec_chl_manager.sec_chl_list_lock);
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    if (ecdh_ctx == NULL) {
        sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
        return CC_FAIL;
    }
    // *exch_param_len = get_exch_buf_len(ecdh_ctx);
    *exch_param_len = ecdh_ctx->local_exch_param_buf_len;
    sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);

    return CC_SUCCESS;
}

int get_enclave_exch_param(size_t session_id, uint8_t *exch_param, size_t exch_param_len)
{
    sc_rdlock(&g_sec_chl_manager.sec_chl_list_lock);
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    if (ecdh_ctx == NULL) {
        sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
        return CC_FAIL;
    }

    int ret = get_exch_buf(ecdh_ctx, exch_param, exch_param_len);
    sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
    return ret;
}

#define RSA_MAX_LEN 1024
int set_enc_key(size_t session_id, uint8_t* data, size_t data_len)
{
    cc_enclave_result_t ret = CC_SUCCESS;

#ifdef GP_ENCLAVE
    // decode base64rul enc_key
    size_t enc_key_len = 0;
    uint8_t *enc_key = kpsecl_base64urldecode((char *)data, data_len, &enc_key_len);

    // unseal enc_key
    size_t dec_key_len = enc_key_len;
    uint8_t *dec_key = (uint8_t *)calloc(1, dec_key_len);
    if (dec_key == NULL) {
        ret = CC_ERROR_SEC_CHL_MEMORY;
        goto end;
    }
    TEE_Result gp_ret = ra_unseal(enc_key, enc_key_len, dec_key, &dec_key_len, TEE_ALG_AES_GCM);
    if (gp_ret != TEE_SUCCESS) {
        PrintInfo(PRINT_ERROR, "unseal enc key failed\n");
        ret = CC_ERROR_SEC_CHL_ENCLAVE_UNSEAL_ENC_KEY;
        goto end;
    }

    // compute private_key
    // dec_key组成形式为：d_len | d | n_len | n | e_len | e ,其中d_len/n_len/e_len分别为四个字节
    uint32_t d_len = *(uint32_t *)dec_key;
    uint32_t n_len = *(uint32_t *)(dec_key + sizeof(uint32_t) + d_len);
    uint32_t e_len = *(uint32_t *)(dec_key + sizeof(uint32_t) + d_len + sizeof(uint32_t) + n_len);
    PrintInfo(PRINT_STRACE, "rsa param len d_len:%u, n_len:%u, e_len:%u\n", d_len, n_len, e_len);

    uint8_t d[RSA_MAX_LEN] = {0};
    uint8_t n[RSA_MAX_LEN] = {0};
    uint8_t e[RSA_MAX_LEN] = {0};
    memcpy(d, dec_key + sizeof(uint32_t), d_len);
    memcpy(n, dec_key + sizeof(uint32_t) + d_len + sizeof(uint32_t), n_len);
    memcpy(e, dec_key + sizeof(uint32_t) + d_len + sizeof(uint32_t) + n_len + sizeof(uint32_t), e_len);

    RSA *rsa_key = RSA_new();
    BIGNUM *modulus = BN_new();
    BIGNUM *pub_exponent = BN_new();
    BIGNUM *pri_exponent = BN_new();
    BN_hex2bn(&modulus, (char *)n);
    BN_hex2bn(&pub_exponent, (char *)e);
    BN_hex2bn(&pri_exponent, (char *)d);
    RSA_set0_key(rsa_key, modulus, pub_exponent, pri_exponent);
    
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    if (ecdh_ctx == NULL) {
        sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
        RSA_free(rsa_key);
        ret = CC_ERROR_SEC_CHL_INVALID_SESSION;
        PrintInfo(PRINT_ERROR, "get ecdh ctx by session id\n");
        goto end;
    }
    ecdh_ctx->svr_rsa_key = rsa_key;
    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
end:
    free(enc_key);
    free(dec_key);

#endif
    return ret;
}

int set_peer_exch_param(size_t session_id, uint8_t* data, size_t data_len)
{
    int ret;
    sec_chl_exch_param_t *peer_exch_param = NULL;
    sec_chl_exch_param_t *local_exch_param = NULL;

    ret = get_exch_param_from_buf(data, data_len, &peer_exch_param);
    if (ret != CC_SUCCESS) {
        return CC_FAIL;
    }
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    ret = get_exch_param_from_buf(ecdh_ctx->local_exch_param_buf,
        ecdh_ctx->local_exch_param_buf_len, &local_exch_param);
    if (ret != CC_SUCCESS) {
        sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
        del_exch_param(peer_exch_param);
        PrintInfo(PRINT_ERROR, "set peer exch param get from buf failed\n");
        return CC_FAIL;
    }
    ret = compute_session_key(ecdh_ctx, local_exch_param, peer_exch_param);
    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);

    del_exch_param(peer_exch_param);
    del_exch_param(local_exch_param);
    if (ret < 0) {
        PrintInfo(PRINT_ERROR, "compute session key failed\n");
        return CC_FAIL;
    }

    return CC_SUCCESS;
}

void del_enclave_sec_chl(size_t session_id)
{
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);

    SEC_CHL_NODE *cur = g_sec_chl_manager.sec_chl_list_head.next;
    SEC_CHL_NODE *pre = &(g_sec_chl_manager.sec_chl_list_head);
    while (cur != NULL) {
        if (cur->session_id == session_id) {
            // remove
            pre->next = cur->next;
            free_sec_chl_node(cur);
            g_sec_chl_manager.count--;
            break;
        }
        pre = cur;
        cur = cur->next;
    }

    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);
}

static void del_enclave_all_sec_chl()
{
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);

    SEC_CHL_NODE *head = &(g_sec_chl_manager.sec_chl_list_head);
    SEC_CHL_NODE *cur = head->next;
    while (cur != NULL) {
        head->next = cur->next;
        free_sec_chl_node(cur);
        cur = head->next;
        g_sec_chl_manager.count--;
    }
    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);

    return;
}

int enclave_start_sec_chl()
{
    sc_init_rwlock(&g_sec_chl_manager.sec_chl_list_lock);
    g_sec_chl_manager.is_init = true;
    return CC_SUCCESS;
}

void enclave_stop_sec_chl()
{
    del_enclave_all_sec_chl();
    sc_fini_rwlock(&g_sec_chl_manager.sec_chl_list_lock);
    g_sec_chl_manager.is_init = false;
    return;
}

int cc_sec_chl_enclave_encrypt(size_t session_id, void *plain, size_t plain_len, void *encrypt, size_t *encrypt_len)
{
    if (plain == NULL || plain_len == 0 || encrypt_len == NULL) {
        PrintInfo(PRINT_ERROR, "sec chl encrypt param error\n");
        return -1;
    }
    size_t need_len = get_encrypted_buf_len(plain_len);
    if (encrypt == NULL || *encrypt_len < need_len) {
        *encrypt_len = need_len;
        return CC_ERROR_SEC_CHL_LEN_NOT_ENOUGH;
    }
    sc_rdlock(&g_sec_chl_manager.sec_chl_list_lock);
    if (!g_sec_chl_manager.is_init) {
        sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
        PrintInfo(PRINT_ERROR, "sec chl encrypt failed, not inited\n");
        return CC_ERROR_SEC_CHL_NOTREADY;
    }
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    if (ecdh_ctx == NULL) {
        sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
        return -1;
    }
    int ret = sec_chl_encrypt(ecdh_ctx, session_id, plain, plain_len, encrypt, encrypt_len);

    sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
    if (ret != CC_SUCCESS) {
        PrintInfo(PRINT_ERROR, "sec chl encrypt failed, ret:%d\n", ret);
        return ret;
    }

    return 0;
}

int cc_sec_chl_enclave_decrypt(size_t session_id, void *encrypt, size_t encrypt_len, void *plain, size_t *plain_len)
{
    if (encrypt == NULL || encrypt_len == 0 || plain_len == NULL) {
        PrintInfo(PRINT_ERROR, "sec chl decrypt param error\n");
        return -1;
    }
    size_t need_len = get_plain_buf_len((uint8_t *)encrypt, encrypt_len);
    if (need_len == 0) {
        return CC_ERROR_SEC_CHL_ENCRYPTED_LEN_INVALID;
    }
    if (plain == NULL || *plain_len < need_len) {
        *plain_len = need_len;
        return CC_ERROR_SEC_CHL_LEN_NOT_ENOUGH;
    }
    sc_rdlock(&g_sec_chl_manager.sec_chl_list_lock);
    if (!g_sec_chl_manager.is_init) {
        sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
        PrintInfo(PRINT_ERROR, "sec chl decrypt failed, not inited\n");
        return CC_ERROR_SEC_CHL_NOTREADY;
    }
    sec_chl_ecdh_ctx_t *ecdh_ctx = get_ecdh_ctx_by_session_id(session_id);
    if (ecdh_ctx == NULL) {
        sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
        return -1;
    }
    int ret = sec_chl_decrypt(ecdh_ctx, session_id, encrypt, encrypt_len, plain, plain_len);

    sc_rdunlock(&g_sec_chl_manager.sec_chl_list_lock);
    if (ret != CC_SUCCESS) {
        PrintInfo(PRINT_ERROR, "sec chl decrypt failed, ret:%d\n", ret);
        return ret;
    }

    return CC_SUCCESS;
}

/** secure channel connection timeout SEL_CHL_CONN_TIMEOUT_CNT * TIMER_INTERVAL seconds
* secure channel receive any msg, reset the counter. When the counter reaches SEL_CHL_CONN_TIMEOUT_CNT
* release the secure channel and resource
*/
static const int64_t SEL_CHL_CONN_TIMEOUT_CNT = 15;

void enclave_check_session_timeout()
{
    sc_wtlock(&g_sec_chl_manager.sec_chl_list_lock);

    SEC_CHL_NODE *pre = &(g_sec_chl_manager.sec_chl_list_head);
    SEC_CHL_NODE *cur = pre->next;

    while (cur != NULL) {
        if (cur->inactive_cnt > SEL_CHL_CONN_TIMEOUT_CNT) {
            pre->next = cur->next;
            PrintInfo(PRINT_WARNING, "sec chl node timeout, session_id:%llu\n", cur->session_id);
            free_sec_chl_node(cur);
            cur = pre->next;
            g_sec_chl_manager.count--;
            continue;
        }
        cur->inactive_cnt++;
        pre = cur;
        cur = cur->next;
    }
    sc_wtunlock(&g_sec_chl_manager.sec_chl_list_lock);

    return;
}
