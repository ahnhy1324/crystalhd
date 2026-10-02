/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual opt-in probe and lifecycle bodies with hardware-free callbacks. */
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_dts_types.h"
struct _BC_DTS_PROC_OUT;
#include "bc_dts_defs.h"
#include "crystalhd_ioctl_limits.h"
#include "crystalhd_fw_if.h"
#include "crystalhd_fw_research.h"
#include "flea/bcm_70015_regs.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { u32 cmd[64], rsp[64], flags, add_data; } BC_FW_CMD;
enum { BC_LINK_INVALID = 0, BC_LINK_INIT = 1, DTS_MODE_INV = -1, BC_PCI_DEVID_FLEA = 0x1615 };
enum { FLEA_PS_ACTIVE, FLEA_PS_LP_COMPLETE };
#define FLEA_GISB_INDIRECT_DATA 0xfffcU
enum { MODULE_STATE_LIVE, MODULE_STATE_COMING, MODULE_STATE_GOING };
#define SHA256_DIGEST_SIZE 32U
#define GFP_KERNEL 0
#define CAP_SYS_RAWIO 17
#define CRYSTALHD_FLEA_FIRMWARE_NAME "bcm70015fw.bin"
#define CRYSTALHD_ENABLE_FW_RESEARCH 1
#define CONFIG_COMPAT 1
#define ERESTARTSYS 512
#define READ_ONCE(value) (value)
#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#define BUILD_BUG_ON(condition) _Static_assert(!(condition), "wire size")
#define IS_ERR(value) ((intptr_t)(value) < 0)
#define PTR_ERR(value) ((int)(intptr_t)(value))
#define __user
#define __init
#define __exit
#define KERN_DEBUG ""
#define KERN_ERR ""
#define printk(...) ((void)0)
#define MISC_DYNAMIC_MINOR 255

static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #value); abort(); } } while (0)
struct rwsem { unsigned readers, writers; };
struct module { int state; };
static struct module module;
#define THIS_MODULE (&module)
struct device { int unused; };
struct pci_dev { struct device dev; unsigned device; };
struct crystalhd_adp;
struct mutex { unsigned held; };
struct spinlock { unsigned held; };
struct crystalhd_hw {
    struct crystalhd_adp *adp;
    struct mutex fwcmd_trans_mutex;
    struct spinlock lock;
    bool dma_fault, fwcmd_pending, fwcmd_poisoned;
    unsigned FleaPowerState;
    BC_STATUS (*pfnDevDRAMRead)(struct crystalhd_hw *, u32, u32, u32 *);
    void *pfnWriteDevRegister;
};
struct crystalhd_cmd {
    struct crystalhd_adp *adp;
    const void *session_owner, *session_lifetime_owner, *session_lifetime_ops;
    bool session_module_pinned, retain_rx_on_suspend;
    void *stream;
    struct crystalhd_hw *hw_ctx;
    int state;
    u32 fw_sequence;
    struct { bool in_use; int mode; } user[4];
};
struct crystalhd_adp {
    bool present, hw_accessible;
    u64 generation;
    unsigned cfg_users;
    struct pci_dev *pdev;
    struct crystalhd_cmd cmds;
    struct rwsem user_lock;
    void *i2o_addr, *mem_addr;
    size_t pci_i2o_len, pci_mem_len;
    void *fill_byte_pool, *elem_pool_head, *ua_map_free_head;
};
struct crystalhd_device_access { struct crystalhd_adp *adp; bool exclusive; };
struct firmware { size_t size; const u8 *data; };
struct crypto_shash { int unused; };
struct shash_desc { struct crypto_shash *tfm; };
struct inode { int unused; };
struct file { void *private_data; };
struct file_operations {
    struct module *owner;
    int (*open)(struct inode *, struct file *);
    int (*release)(struct inode *, struct file *);
    long (*unlocked_ioctl)(struct file *, unsigned, unsigned long);
    long (*compat_ioctl)(struct file *, unsigned, unsigned long);
    void *llseek;
};
struct miscdevice { int minor; const char *name; const struct file_operations *fops; unsigned mode; };
#define no_llseek NULL
#define compat_ptr(value) ((void *)(uintptr_t)(value))

static struct crystalhd_adp adp, *g_adp_info;
static struct pci_dev pci;
static struct crystalhd_hw hardware;
static struct rwsem chd_device_lock;
static u64 chd_device_generation;
static struct firmware firmware;
static struct crypto_shash hash_tfm;
static const u8 firmware_bytes[] = { 1, 2, 3, 4 };
static bool privileged, bad_digest, copy_in_error, copy_out_error;
static bool acquire_retains, release_retains;
static int request_error, crypto_error, digest_error, allocation_fail, nonseekable_error;
static int allocations, live_allocations, request_count, firmware_release_count;
static unsigned copy_in_count, copy_out_count;
static int hash_count, hash_free_count, acquire_count, download_count, release_count;
static BC_STATUS acquire_status, download_status, release_status;
static BC_STATUS command_status[5];
static unsigned command_count, wrong_command, wrong_sequence, wrong_channel;
static u32 active_selector;
static bool codec_rejection, unknown_command_reply;
static const u32 raw_commands[] = {
    eCMD_C011_DEC_CHAN_SCALING_FILTERS, eCMD_C011_DEC_CHAN_PIC_CAPTURE,
    eCMD_C011_DEC_CHAN_SET_CSC, eCMD_C011_DEC_CHAN_SET_FGT,
    eCMD_C011_DEC_CHAN_CUSTOM_VIDOUT, eCMD_C011_DEC_CHAN_FILL_PIC_BUF,
};
static int remove_at, step;
static const void *last_owner;
static unsigned lifecycle[16], lifecycle_count;
static int v4l2_error, misc_error, pci_error;
static int bc_chd_driver;
static unsigned lock_attempts;
static unsigned metadata_race;
static unsigned read_count, read_fail_at, read_mutate_at, read_mutation;
static unsigned read_mismatch_stage, read_mismatch_word;
static bool read_padding;
static bool controller_reads, controller_only;
static u32 controller_roots[2];
static BC_STATUS read_status;
static int transaction_error;
static unsigned transaction_mutation, transaction_count;
static unsigned transaction_error_at, transaction_mutation_at;
static BC_STATUS read_mock(struct crystalhd_hw *hw, u32 offset, u32 count, u32 *words);

static void down_read(struct rwsem *lock)
{
    CHECK(!lock->writers); lock->readers++; lock_attempts++;
    if (lock == &adp.user_lock && metadata_race) {
        switch (metadata_race) {
        case 1: adp.present = false; break;
        case 2: adp.generation = 0; break;
        case 3: adp.generation++; break;
        case 4: chd_device_generation++; break;
        case 5: adp.hw_accessible = false; break;
        default: CHECK(false);
        }
        metadata_race = 0;
    }
}
static void up_read(struct rwsem *lock)
{ CHECK(lock->readers); lock->readers--; }
static void down_write(struct rwsem *lock)
{ CHECK(!lock->writers && !lock->readers); lock->writers++; lock_attempts++; }
static void up_write(struct rwsem *lock)
{ CHECK(lock->writers == 1); lock->writers--; }
#define lockdep_assert_held_write(lock) CHECK((lock)->writers == 1)
static void barrier(void)
{ CHECK(chd_device_lock.readers == 1 && adp.user_lock.writers == 1); }
static void unlocked(void)
{ CHECK(!chd_device_lock.readers && !adp.user_lock.writers && !adp.user_lock.readers);
  CHECK(!hardware.lock.held && !hardware.fwcmd_trans_mutex.held); }
static void state_mutate(unsigned mutation)
{
    switch (mutation) {
    case 0: break;
    case 1: adp.present = false; break;
    case 2: adp.generation++; break;
    case 3: adp.hw_accessible = false; break;
    case 4: hardware.dma_fault = true; break;
    case 5: hardware.fwcmd_pending = true; break;
    case 6: hardware.fwcmd_poisoned = true; break;
    case 7: hardware.FleaPowerState = FLEA_PS_LP_COMPLETE; break;
    case 8: adp.cmds.session_owner = &pci; break;
    case 9: adp.pci_mem_len = 0x3ad3; break;
    case 10: hardware.adp = NULL; break;
    default: CHECK(false);
    }
}
static int mutex_lock_interruptible(struct mutex *lock)
{
    barrier(); CHECK(lock == &hardware.fwcmd_trans_mutex && !lock->held && !hardware.lock.held);
    transaction_count++;
    if (transaction_error && (!transaction_error_at || transaction_count == transaction_error_at))
        return transaction_error;
    lock->held = 1;
    if (!transaction_mutation_at || transaction_count == transaction_mutation_at)
        state_mutate(transaction_mutation);
    return 0;
}
static void mutex_unlock(struct mutex *lock)
{ CHECK(lock == &hardware.fwcmd_trans_mutex && lock->held == 1 && !hardware.lock.held); lock->held = 0; }
#define spin_lock_irqsave(lock, flags) do { \
    CHECK(!(lock)->held && hardware.fwcmd_trans_mutex.held == 1); \
    (lock)->held = 1; (flags) = 0x1234; \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    CHECK((lock)->held == 1 && (flags) == 0x1234); (lock)->held = 0; \
} while (0)
#define lockdep_assert_held(lock) CHECK((lock)->held == 1)
static void advance(void)
{ barrier(); if (++step == remove_at) adp.present = false; }
static bool capable(unsigned capability)
{ CHECK(capability == CAP_SYS_RAWIO); return privileged; }
static void *kzalloc(size_t size, int flags)
{
    void *ptr;
    CHECK(flags == GFP_KERNEL);
    allocations++;
    if (allocation_fail == allocations) return NULL;
    ptr = calloc(1, size);
    CHECK(ptr);
    live_allocations++;
    return ptr;
}
static void kfree(void *ptr)
{ if (ptr) { CHECK(live_allocations > 0); live_allocations--; free(ptr); } }
static struct crypto_shash *crypto_alloc_shash(const char *name, int type, int mask)
{ barrier(); CHECK(!strcmp(name, "sha256") && !type && !mask); return crypto_error ? (void *)(intptr_t)crypto_error : &hash_tfm; }
static size_t crypto_shash_descsize(struct crypto_shash *tfm)
{ CHECK(tfm == &hash_tfm); return 16; }
static void crypto_free_shash(struct crypto_shash *tfm)
{ barrier(); CHECK(tfm == &hash_tfm); hash_free_count++; }
static int crypto_shash_digest(struct shash_desc *desc, const u8 *data, size_t size, u8 *digest);
static int request_firmware(const struct firmware **out, const char *name, struct device *dev)
{
    barrier(); CHECK(!strcmp(name, CRYSTALHD_FLEA_FIRMWARE_NAME) && dev == &pci.dev);
    CHECK(!acquire_count && !download_count && !command_count);
    request_count++; advance();
    if (request_error) return request_error;
    *out = &firmware;
    return 0;
}
static void release_firmware(const struct firmware *fw)
{ barrier(); CHECK(fw == &firmware); firmware_release_count++; }
static void retained_clear(struct crystalhd_cmd *ctx)
{
    ctx->session_owner = ctx->session_lifetime_owner = ctx->session_lifetime_ops = NULL;
    ctx->session_module_pinned = false;
    ctx->hw_ctx = NULL; ctx->stream = NULL;
}
static BC_STATUS crystalhd_session_acquire_locked(struct crystalhd_cmd *ctx, const void *owner)
{
    barrier(); CHECK(ctx == &adp.cmds && owner && hash_count == 1 && !bad_digest && !digest_error);
    acquire_count++; last_owner = owner; advance();
    if (acquire_status == BC_STS_SUCCESS) {
        ctx->session_owner = owner; ctx->session_module_pinned = true;
        ctx->hw_ctx = &hardware; ctx->fw_sequence = 0;
    } else if (acquire_retains) {
        ctx->session_module_pinned = true; ctx->hw_ctx = &hardware;
    }
    return acquire_status;
}
static BC_STATUS crystalhd_fw_download_locked(struct crystalhd_cmd *ctx, const void *owner,
                                             const void *bytes, size_t size)
{
    barrier(); CHECK(ctx == &adp.cmds && owner == last_owner && ctx->session_owner == owner);
    CHECK(bytes == firmware.data && size == firmware.size && hash_count == 1);
    CHECK(download_count++ == 0); advance();
    if (download_status == BC_STS_SUCCESS) ctx->state = BC_LINK_INIT;
    return download_status;
}
static void check_payload(const BC_FW_CMD *cmd);
static BC_STATUS crystalhd_fw_exec_locked(struct crystalhd_cmd *ctx, const void *owner, BC_FW_CMD *cmd)
{
    static const u32 commands[] = {eCMD_C011_INIT, eCMD_C011_GET_VERSION,
        eCMD_C011_DEC_CHAN_OPEN, eCMD_C011_DEC_CHAN_STATUS, eCMD_C011_DEC_CHAN_CLOSE};
    unsigned index = command_count++;
    barrier(); CHECK(index < 5 && ctx == &adp.cmds && owner == last_owner && ctx->session_owner == owner);
    CHECK(index < (active_selector == 1 ? 2U : active_selector == 2 ? 5U : active_selector <= 5 ? 4U : 3U));
    CHECK(cmd->cmd[0] == (active_selector >= 6 && index == 2 ? raw_commands[active_selector - 6] :
        active_selector >= 3 && active_selector <= 5 && index == 3 ? eCMD_C011_DEC_CHAN_CLOSE : commands[index]));
    check_payload(cmd);
    CHECK(cmd->cmd[1] == index + 1);
    cmd->rsp[0] = cmd->cmd[0] ^ (index + 1 == wrong_command ? 1U : 0U);
    cmd->rsp[1] = cmd->cmd[1] + (index + 1 == wrong_sequence ? 1U : 0U);
    cmd->rsp[2] = command_status[index] == BC_STS_FW_CMD_ERR ? 0x1234 : 0;
    cmd->rsp[3] = index + 1 == wrong_channel ? 7 : 0;
    if (codec_rejection && index == 2) {
        CHECK(command_status[index] == BC_STS_FW_CMD_ERR);
        cmd->rsp[2] = cmd->rsp[3] = UINT32_MAX;
    }
    cmd->rsp[63] = 0xfeed;
    if (unknown_command_reply && index == 2) {
        CHECK(active_selector >= 6 && command_status[index] == BC_STS_SUCCESS);
        memset(cmd->rsp, 0, sizeof(cmd->rsp));
        cmd->rsp[0] = cmd->cmd[0];
    }
    advance();
    return command_status[index];
}
static BC_STATUS crystalhd_session_release_locked(struct crystalhd_cmd *ctx, const void *owner)
{
    barrier(); CHECK(ctx == &adp.cmds && owner == last_owner && ctx->session_owner == owner);
    release_count++; advance();
    if (release_status == BC_STS_SUCCESS && !release_retains) retained_clear(ctx);
    return release_status;
}
static int nonseekable_open(struct inode *inode, struct file *file)
{ unlocked(); CHECK(file->private_data); return nonseekable_error; }
static unsigned copy_from_user(void *to, const void *from, size_t size)
{ unlocked(); copy_in_count++; if (copy_in_error) return 1; memcpy(to, from, size); return 0; }
static unsigned copy_to_user(void *to, const void *from, size_t size)
{ unlocked(); copy_out_count++; if (copy_out_error) return 1; memcpy(to, from, size); return 0; }
static void lifecycle_event(unsigned event)
{ CHECK(lifecycle_count < ARRAY_SIZE(lifecycle)); lifecycle[lifecycle_count++] = event; }
static int misc_register(struct miscdevice *device)
{
    CHECK(device->mode == 0600 && device->fops->owner == THIS_MODULE);
    CHECK(device->minor == MISC_DYNAMIC_MINOR && !strcmp(device->name, "crystalhd-fw-research"));
    lifecycle_event(2); return misc_error;
}
static void misc_deregister(struct miscdevice *device)
{ CHECK(device->mode == 0600); lifecycle_event(4); }
static int crystalhd_v4l2_init(void) { lifecycle_event(1); return v4l2_error; }
static void crystalhd_v4l2_cleanup(void) { lifecycle_event(6); }
static int pci_register_driver(void *driver)
{ CHECK(driver == &bc_chd_driver); lifecycle_event(3); return pci_error; }
static void pci_unregister_driver(void *driver)
{ CHECK(driver == &bc_chd_driver); lifecycle_event(5); }

#include "device-functions.h"
#include "status-functions.h"
#include "hw-transaction-functions.h"
#include "probe-functions.h"
#include "module-functions.h"
#undef CRYSTALHD_ENABLE_FW_RESEARCH
#define chd_dec_module_init default_module_init
#define chd_dec_module_cleanup default_module_cleanup
#include "module-functions.h"
#undef chd_dec_module_init
#undef chd_dec_module_cleanup

static int crypto_shash_digest(struct shash_desc *desc, const u8 *data, size_t size, u8 *digest)
{
    barrier(); CHECK(desc->tfm == &hash_tfm && data == firmware.data && size == firmware.size);
    CHECK(!acquire_count && !download_count && !command_count);
    hash_count++; memcpy(digest, crystalhd_fw_research_sha256, 32);
    if (bad_digest) digest[0] ^= 1;
    if (digest_error) memset(digest, 0x5a, 16); /* Failed provider wrote a partial digest. */
    advance();
    return digest_error;
}

static void check_payload(const BC_FW_CMD *cmd)
{
    u32 expected[64] = {0};
    expected[0] = cmd->cmd[0]; expected[1] = cmd->cmd[1];
    switch (cmd->cmd[0]) {
    case eCMD_C011_INIT:
        expected[2] = 64; expected[3] = 200000000; expected[4] = 38400;
        expected[5] = 3; expected[6] = 1; expected[8] = 2; expected[9] = 1;
        break;
    case eCMD_C011_GET_VERSION: case eCMD_C011_DEC_CHAN_STATUS:
    case eCMD_C011_DEC_CHAN_SCALING_FILTERS: case eCMD_C011_DEC_CHAN_PIC_CAPTURE:
    case eCMD_C011_DEC_CHAN_SET_CSC: case eCMD_C011_DEC_CHAN_SET_FGT:
    case eCMD_C011_DEC_CHAN_CUSTOM_VIDOUT: case eCMD_C011_DEC_CHAN_FILL_PIC_BUF:
        break;
    case eCMD_C011_DEC_CHAN_OPEN:
        expected[4] = 1;
        expected[9] = active_selector == 3 ? 2U : active_selector == 4 ? 3U : active_selector == 5 ? 5U : 0U;
        break;
    case eCMD_C011_DEC_CHAN_CLOSE: expected[3] = 1; break;
    default: CHECK(false);
    }
    CHECK(!memcmp(cmd->cmd, expected, sizeof(expected)));
    memset(expected, 0, sizeof(expected));
    CHECK(!memcmp(cmd->rsp, expected, sizeof(expected)) && !cmd->flags && !cmd->add_data);
}

static struct crystalhd_fw_research_request request(void)
{
    struct crystalhd_fw_research_request req = {0};
    req.version = CRYSTALHD_FW_RESEARCH_VERSION;
    req.size = sizeof(struct crystalhd_fw_research_result);
    req.selector = active_selector;
    return req;
}
static void reset(void)
{
    unsigned i;
    CHECK(!live_allocations); unlocked();
    memset(&adp, 0, sizeof(adp)); memset(&pci, 0, sizeof(pci));
    memset(&hardware, 0, sizeof(hardware)); hardware.adp = &adp;
    hardware.pfnDevDRAMRead = read_mock; hardware.pfnWriteDevRegister = &pci;
    hardware.FleaPowerState = FLEA_PS_ACTIVE;
    adp.i2o_addr = adp.mem_addr = &pci; adp.pci_i2o_len = 0x10000; adp.pci_mem_len = 0x10000;
    g_adp_info = &adp; adp.pdev = &pci; adp.present = adp.hw_accessible = true;
    adp.generation = chd_device_generation = 42; adp.cmds.adp = &adp;
    for (i = 0; i < ARRAY_SIZE(adp.cmds.user); i++) adp.cmds.user[i].mode = DTS_MODE_INV;
    pci.device = BC_PCI_DEVID_FLEA; module.state = MODULE_STATE_LIVE; privileged = true;
    firmware.data = firmware_bytes; firmware.size = sizeof(firmware_bytes);
    bad_digest = copy_in_error = copy_out_error = acquire_retains = release_retains = false;
    request_error = crypto_error = digest_error = allocation_fail = nonseekable_error = 0;
    allocations = request_count = firmware_release_count = hash_count = hash_free_count = 0;
    acquire_count = download_count = release_count = 0;
    acquire_status = download_status = release_status = BC_STS_SUCCESS;
    memset(command_status, 0, sizeof(command_status));
    command_count = wrong_command = wrong_sequence = wrong_channel = lock_attempts = metadata_race = 0;
    copy_in_count = copy_out_count = 0;
    remove_at = step = 0; lifecycle_count = 0; v4l2_error = misc_error = pci_error = 0;
    last_owner = NULL;
    active_selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
    codec_rejection = unknown_command_reply = false;
    read_count = read_fail_at = read_mutate_at = read_mutation = 0;
    read_mismatch_stage = read_mismatch_word = 0; read_padding = false;
    controller_reads = controller_only = false;
    controller_roots[0] = controller_roots[1] = 0xd6000;
    read_status = BC_STS_IO_ERROR; transaction_error = 0; transaction_mutation = 0;
    transaction_count = transaction_error_at = transaction_mutation_at = 0;
}
static struct crystalhd_fw_research_result run(void)
{
    struct crystalhd_fw_research_result result;
    struct crystalhd_fw_research_request req = request();
    crystalhd_fw_research_run(42, &req, &result);
    unlocked(); CHECK(!live_allocations);
    CHECK(result.request.version == req.version && result.generation == 42);
    return result;
}
static void no_hardware(void)
{ CHECK(!request_count && !hash_count && !acquire_count && !download_count && !command_count && !release_count && !read_count); }

static void test_lifecycle(void)
{
    static const unsigned success[] = {1, 2, 3, 4, 5, 6};
    static const unsigned pci_fail[] = {1, 2, 3, 4, 6};
    static const unsigned misc_fail[] = {1, 2, 6};
    static const unsigned default_success[] = {1, 3, 5, 6};
    static const unsigned default_fail[] = {1, 3, 6};
    reset(); CHECK(chd_dec_module_init() == 0); chd_dec_module_cleanup();
    CHECK(lifecycle_count == ARRAY_SIZE(success) && !memcmp(lifecycle, success, sizeof(success)));
    reset(); pci_error = -EIO; CHECK(chd_dec_module_init() == -EIO);
    CHECK(lifecycle_count == ARRAY_SIZE(pci_fail) && !memcmp(lifecycle, pci_fail, sizeof(pci_fail)));
    reset(); misc_error = -ENOMEM; CHECK(chd_dec_module_init() == -ENOMEM);
    CHECK(lifecycle_count == ARRAY_SIZE(misc_fail) && !memcmp(lifecycle, misc_fail, sizeof(misc_fail)));
    reset(); v4l2_error = -ENOMEM; CHECK(chd_dec_module_init() == -ENOMEM);
    CHECK(lifecycle_count == 1 && lifecycle[0] == 1);
    reset(); CHECK(default_module_init() == 0); default_module_cleanup();
    CHECK(lifecycle_count == ARRAY_SIZE(default_success) && !memcmp(lifecycle, default_success, sizeof(default_success)));
    reset(); pci_error = -EIO; CHECK(default_module_init() == -EIO);
    CHECK(lifecycle_count == ARRAY_SIZE(default_fail) && !memcmp(lifecycle, default_fail, sizeof(default_fail)));
    reset(); v4l2_error = -ENOMEM; CHECK(default_module_init() == -ENOMEM);
    CHECK(lifecycle_count == 1 && lifecycle[0] == 1);
    CHECK(crystalhd_fw_research_fops.open == crystalhd_fw_research_open &&
          crystalhd_fw_research_fops.release == crystalhd_fw_research_release &&
          crystalhd_fw_research_fops.unlocked_ioctl == crystalhd_fw_research_ioctl &&
          crystalhd_fw_research_fops.compat_ioctl == crystalhd_fw_research_compat_ioctl);
}

static void test_admission(void)
{
    struct file file = {0}; struct inode inode = {0};
    struct crystalhd_fw_research_result result;
    struct crystalhd_fw_research_request req;
    u64 generation;
    unsigned i;
    int state;
    for (state = MODULE_STATE_COMING; state <= MODULE_STATE_GOING; state++) {
        reset(); module.state = state;
        CHECK(crystalhd_fw_research_open(&inode, &file) == -EAGAIN);
        result = run(); CHECK(result.status == -EAGAIN && !allocations && !lock_attempts); no_hardware();
        CHECK(crystalhd_fw_research_ioctl(&file, 0, 0) == -EAGAIN);
    }
    reset(); privileged = false;
    CHECK(crystalhd_fw_research_open(&inode, &file) == -EPERM);
    result = run(); CHECK(result.status == -EPERM && !allocations && !lock_attempts); no_hardware();
    CHECK(crystalhd_fw_research_ioctl(&file, 0, 0) == -EPERM);
    for (i = 0; i < 8; i++) {
        reset(); req = request();
        if (i == 0) req.version++;
        else if (i == 1) req.size--;
        else if (i == 2) req.selector = 0;
        else if (i == 3) req.flags = 1;
        else req.reserved[i - 4] = 1;
        memset(&result, 0xa5, sizeof(result));
        crystalhd_fw_research_run(42, &req, &result);
        CHECK(result.status == -EINVAL && !allocations && !lock_attempts); no_hardware();
    }
    reset(); CHECK(crystalhd_fw_research_generation(NULL) == -EINVAL); no_hardware();
    for (i = 0; i < 5; i++) {
        reset(); generation = 99;
        if (i == 0) g_adp_info = NULL;
        if (i == 1) adp.present = false;
        if (i == 2) adp.generation = 0;
        if (i == 3) adp.generation++;
        if (i == 4) adp.hw_accessible = false;
        CHECK(crystalhd_fw_research_generation(&generation) == (i == 4 ? -EAGAIN : -ENODEV));
        CHECK(!generation); unlocked(); no_hardware();
    }
    /* Publish cancellation/PM or a changed generation while metadata lookup
     * enters the user lock: the second validation must not publish a binding. */
    for (i = 1; i <= 5; i++) {
        reset(); metadata_race = i;
        CHECK(crystalhd_fw_research_open(&inode, &file) == (i == 5 ? -EAGAIN : -ENODEV));
        CHECK(!file.private_data && !live_allocations && !metadata_race);
        unlocked(); no_hardware();
    }
    reset(); CHECK(crystalhd_fw_research_open(&inode, &file) == 0); no_hardware();
    CHECK(((struct crystalhd_fw_research_file *)file.private_data)->generation == 42);
    req = request(); chd_device_generation++;
    memset(&result, 0, sizeof(result));
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, (unsigned long)&result) == -EINVAL);
    result.request = req;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, (unsigned long)&result) == 0);
    CHECK(result.status == -ENODEV); no_hardware();
    CHECK(crystalhd_fw_research_release(&inode, &file) == 0 && !file.private_data && !live_allocations);
    reset(); allocation_fail = 1; CHECK(crystalhd_fw_research_open(&inode, &file) == -ENOMEM); no_hardware();
    reset(); adp.hw_accessible = false; CHECK(crystalhd_fw_research_open(&inode, &file) == -EAGAIN);
    CHECK(!file.private_data && !live_allocations); no_hardware();
    reset(); nonseekable_error = -EINVAL;
    CHECK(crystalhd_fw_research_open(&inode, &file) == -EINVAL);
    CHECK(!file.private_data && !live_allocations); no_hardware();
    reset(); adp.cfg_users = 1; adp.cmds.state = 9;
    CHECK(crystalhd_fw_research_open(&inode, &file) == 0); no_hardware();
    result.request = request(); adp.hw_accessible = false;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, (unsigned long)&result) == 0);
    CHECK(result.status == -EAGAIN && !result.retained); no_hardware();
    CHECK(crystalhd_fw_research_release(&inode, &file) == 0 && !live_allocations);
}

static void test_idle(void)
{
    struct crystalhd_fw_research_result result;
    unsigned gate, user;
    for (gate = 0; gate < 16; gate++) {
        reset();
        switch (gate) {
        case 0: adp.present = false; break;
        case 1: adp.cmds.adp = NULL; break;
        case 2: adp.hw_accessible = false; break;
        case 3: pci.device = 0x1612; break;
        case 4: adp.cfg_users = 1; break;
        case 5: adp.cmds.state = 1; break;
        case 6: adp.cmds.retain_rx_on_suspend = true; break;
        case 7: adp.cmds.session_owner = &pci; break;
        case 8: adp.cmds.session_module_pinned = true; break;
        case 9: adp.cmds.session_lifetime_owner = &pci; break;
        case 10: adp.cmds.session_lifetime_ops = &pci; break;
        case 11: adp.cmds.stream = &pci; break;
        case 12: adp.cmds.hw_ctx = &hardware; break;
        case 13: adp.fill_byte_pool = &pci; break;
        case 14: adp.elem_pool_head = &pci; break;
        case 15: adp.ua_map_free_head = &pci; break;
        }
        result = run();
        CHECK(result.status == (gate < 2 ? -ENODEV : gate == 2 ? -EAGAIN : gate == 3 ? -EOPNOTSUPP : -EBUSY));
        CHECK(!result.retained); /* Refusal does not claim another owner's resources. */
        no_hardware();
    }
    for (user = 0; user < ARRAY_SIZE(adp.cmds.user); user++) {
        reset(); adp.cmds.user[user].in_use = true; result = run(); CHECK(result.status == -EBUSY); no_hardware();
        reset(); adp.cmds.user[user].mode = 0; result = run(); CHECK(result.status == -EBUSY); no_hardware();
    }
}

static void test_hash(void)
{
    struct crystalhd_fw_research_result result;
    u8 digest[32];
    const u8 zero[32] = {0};
    bool completed = true;
    unsigned failure;
    reset(); memset(digest, 0xa5, sizeof(digest));
    CHECK(crystalhd_fw_research_hash(NULL, digest, &completed) == -EINVAL);
    CHECK(!completed && !memcmp(digest, zero, sizeof(digest))); no_hardware();
    for (failure = 0; failure < 9; failure++) {
        reset();
        switch (failure) {
        case 0: request_error = -ENOENT; break;
        case 1: firmware.data = NULL; break;
        case 2: firmware.size = 0; break;
        case 3: firmware.size = CRYSTALHD_MAX_FIRMWARE_SIZE + 1; break;
        case 4: crypto_error = -ENOENT; break;
        case 5: allocation_fail = 1; break;
        case 6: digest_error = -EIO; break;
        case 7: bad_digest = true; break;
        case 8: remove_at = 2; break;
        }
        result = run();
        CHECK(result.status == (failure == 0 || failure == 4 ? -ENOENT : failure < 4 ? -EINVAL :
              failure == 5 ? -ENOMEM : failure == 6 ? -EIO : failure == 7 ? -EKEYREJECTED : -ENODEV));
        CHECK(request_count == 1 && firmware_release_count == (failure != 0));
        CHECK(!acquire_count && !download_count && !command_count && !release_count);
        CHECK(hash_free_count == (failure >= 5));
        CHECK(result.firmware_hash_valid == (failure >= 7));
        if (failure < 7) CHECK(!memcmp(result.firmware_sha256, zero, 32));
        else CHECK((memcmp(result.firmware_sha256, crystalhd_fw_research_sha256, 32) != 0) == (failure == 7));
    }
}

static void test_commands(void)
{
    static const u32 commands[] = {eCMD_C011_INIT, eCMD_C011_GET_VERSION,
        eCMD_C011_DEC_CHAN_OPEN, eCMD_C011_DEC_CHAN_STATUS, eCMD_C011_DEC_CHAN_CLOSE};
    static const BC_STATUS failures[] = {BC_STS_IO_ERROR, BC_STS_TIMEOUT,
        BC_STS_IO_USER_ABORT, BC_STS_CMD_CANCELLED, BC_STS_BUSY, BC_STS_PWR_MGMT,
        BC_STS_FW_CMD_ERR};
    struct crystalhd_fw_research_result result;
    struct crystalhd_fw_research_request req;
    BC_FW_CMD payload;
    unsigned position, failure, i;
    reset(); result = run(); CHECK(!result.status && result.command_count == 5 && !result.retained);
    CHECK(download_count == 1 && release_count == 1 && firmware_release_count == 1 && !live_allocations);
    for (i = 0; i < 5; i++) {
        CHECK(result.replies[i].command == commands[i] && result.replies[i].sequence == i + 1);
        CHECK(result.replies[i].raw_response_valid && result.replies[i].header_matches && result.replies[i].response[63] == 0xfeed);
        memset(&payload, 0xa5, sizeof(payload));
        CHECK(!crystalhd_fw_research_payload(&payload, commands[i], i + 1, active_selector)); check_payload(&payload);
    }
    CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_START_VIDEO, 1, active_selector) == -EINVAL);
    CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_PIC_CAPTURE, 1, active_selector) == -EINVAL);
    CHECK(crystalhd_fw_research_payload(&payload, 0xdeadbeef, 1, active_selector) == -EINVAL);
    reset(); active_selector = CRYSTALHD_FW_RESEARCH_VERSION_ONLY; req = request();
    crystalhd_fw_research_run(42, &req, &result); CHECK(!result.status && command_count == 2 && release_count == 1);
    for (position = 0; position < 5; position++) {
        for (failure = 0; failure < ARRAY_SIZE(failures); failure++) {
            reset(); command_status[position] = failures[failure]; result = run();
            CHECK(result.status == crystalhd_status_to_errno(failures[failure]));
            CHECK(command_count == position + 1 && result.command_count == position + 1 && release_count == 1);
            CHECK(result.replies[position].transport_status == (u32)failures[failure]);
            CHECK(result.replies[position].raw_response_valid == (failures[failure] == BC_STS_FW_CMD_ERR));
            CHECK(result.replies[position].header_matches == (failures[failure] == BC_STS_FW_CMD_ERR));
            CHECK(result.replies[position].response[63] == (failures[failure] == BC_STS_FW_CMD_ERR ? 0xfeedU : 0U));
            CHECK(!result.retained && firmware_release_count == 1);
        }
        reset(); wrong_command = position + 1; result = run();
        CHECK(result.status == -EPROTO && command_count == position + 1 && release_count == 1);
        CHECK(result.replies[position].raw_response_valid && !result.replies[position].header_matches);
        reset(); wrong_sequence = position + 1; result = run();
        CHECK(result.status == -EPROTO && command_count == position + 1 && release_count == 1);
    }
    reset(); wrong_channel = 3; result = run();
    CHECK(result.status == -EPROTO && command_count == 3 && release_count == 1 && result.replies[2].header_matches);
    reset(); adp.cmds.fw_sequence = 5; memset(&result, 0, sizeof(result)); result.request = request();
    CHECK(crystalhd_fw_research_command(&adp.cmds, &result, eCMD_C011_INIT) == -EOVERFLOW && !command_count);
    adp.cmds.fw_sequence = 0; result.command_count = 5;
    CHECK(crystalhd_fw_research_command(&adp.cmds, &result, eCMD_C011_INIT) == -EOVERFLOW && !command_count);
}

static void test_named_controls(void)
{
    static const u32 commands[] = {eCMD_C011_INIT, eCMD_C011_GET_VERSION,
        eCMD_C011_DEC_CHAN_OPEN, eCMD_C011_DEC_CHAN_CLOSE};
    static const BC_STATUS failures[] = {BC_STS_IO_ERROR, BC_STS_TIMEOUT,
        BC_STS_IO_USER_ABORT, BC_STS_CMD_CANCELLED, BC_STS_BUSY, BC_STS_PWR_MGMT,
        BC_STS_FW_CMD_ERR};
    static const u32 unknown[] = {0, 12, UINT32_MAX};
    struct crystalhd_fw_research_result result;
    struct crystalhd_fw_research_request req;
    BC_FW_CMD payload;
    unsigned selector, position, failure, i, remove;

    for (selector = 3; selector <= 5; selector++) {
        reset(); active_selector = selector; result = run();
        CHECK(!result.status && result.command_count == 4 && command_count == 4 && !result.retained);
        CHECK(release_count == 1 && firmware_release_count == 1);
        for (i = 0; i < ARRAY_SIZE(commands); i++) {
            CHECK(result.replies[i].command == commands[i] && result.replies[i].sequence == i + 1);
            CHECK(result.replies[i].raw_response_valid && result.replies[i].header_matches);
            memset(&payload, 0xa5, sizeof(payload));
            CHECK(!crystalhd_fw_research_payload(&payload, commands[i], i + 1, selector));
            check_payload(&payload);
        }
        CHECK(result.replies[3].command == eCMD_C011_DEC_CHAN_CLOSE && result.replies[3].sequence == 4);
        CHECK(!memcmp(&result.replies[4], &(struct crystalhd_fw_research_reply){0}, sizeof(result.replies[4])));
        for (position = 0; position < ARRAY_SIZE(commands); position++) {
            for (failure = 0; failure < ARRAY_SIZE(failures); failure++) {
                reset(); active_selector = selector; command_status[position] = failures[failure]; result = run();
                CHECK(result.status == crystalhd_status_to_errno(failures[failure]));
                CHECK(command_count == position + 1 && result.command_count == position + 1 && release_count == 1);
                CHECK(result.replies[position].transport_status == (u32)failures[failure]);
                CHECK(result.replies[position].raw_response_valid == (failures[failure] == BC_STS_FW_CMD_ERR));
                CHECK(result.replies[position].header_matches == (failures[failure] == BC_STS_FW_CMD_ERR));
                if (failures[failure] != BC_STS_FW_CMD_ERR)
                    CHECK(!memcmp(result.replies[position].response, (u32[64]){0}, sizeof(result.replies[position].response)));
                CHECK(!result.retained && firmware_release_count == 1);
            }
            reset(); active_selector = selector; wrong_command = position + 1; result = run();
            CHECK(result.status == -EPROTO && command_count == position + 1 && release_count == 1);
            CHECK(result.replies[position].raw_response_valid && !result.replies[position].header_matches);
            reset(); active_selector = selector; wrong_sequence = position + 1; result = run();
            CHECK(result.status == -EPROTO && command_count == position + 1 && release_count == 1);
        }
        reset(); active_selector = selector; command_status[2] = BC_STS_FW_CMD_ERR; codec_rejection = true;
        result = run();
        CHECK(result.status == -EIO && result.command_count == 3 && command_count == 3 && release_count == 1);
        CHECK(result.replies[2].raw_response_valid && result.replies[2].header_matches);
        CHECK(result.replies[2].response[2] == UINT32_MAX && result.replies[2].response[3] == UINT32_MAX);
        reset(); active_selector = selector; wrong_channel = 3; result = run();
        CHECK(result.status == -EPROTO && command_count == 3 && result.replies[2].header_matches && release_count == 1);
        for (i = 0; i < 4; i++) {
            reset(); active_selector = selector;
            if (i == 0) acquire_status = BC_STS_BUSY;
            if (i == 1) { acquire_status = BC_STS_IO_ERROR; acquire_retains = true; }
            if (i == 2) download_status = BC_STS_IO_ERROR;
            if (i == 3) { release_status = BC_STS_IO_ERROR; release_retains = true; }
            result = run();
            CHECK(result.status == (i == 0 ? -EBUSY : -EIO));
            CHECK(result.retained == (i == 1 || i == 3));
            CHECK(command_count == (i == 3 ? 4U : 0U) && release_count == (i >= 2));
        }
        reset(); active_selector = selector; release_retains = true; result = run();
        CHECK(result.command_count == 4 && !result.cleanup_status && result.retained);
        reset(); active_selector = selector; command_status[2] = BC_STS_TIMEOUT;
        release_status = BC_STS_IO_ERROR; release_retains = true; result = run();
        CHECK(result.status == -ETIMEDOUT && result.command_count == 3 && result.retained && result.cleanup_status == BC_STS_IO_ERROR);
        for (remove = 1; remove <= 8; remove++) {
            reset(); active_selector = selector; remove_at = remove; result = run();
            CHECK(result.status == -ENODEV && !release_count && firmware_release_count == 1);
            CHECK(result.retained == (remove >= 3) && !result.cleanup_attempted);
            CHECK(command_count == (remove > 4 ? remove - 4 : 0));
            if (remove >= 3) CHECK(result.cleanup_status == BC_STS_CMD_CANCELLED && adp.cmds.session_module_pinned);
        }
        for (i = 0; i < 6; i++) {
            reset(); active_selector = selector; req = request();
            if (i == 0) req.flags = 2; /* No arbitrary algorithm override. */
            else if (i <= 4) req.reserved[i - 1] = 8;
            else req.selector = 12;
            crystalhd_fw_research_run(42, &req, &result);
            CHECK(result.status == -EINVAL && !allocations && !lock_attempts); no_hardware();
        }
    }
    for (i = 0; i < ARRAY_SIZE(unknown); i++) {
        reset(); req = request(); req.selector = unknown[i];
        crystalhd_fw_research_run(42, &req, &result);
        CHECK(result.status == -EINVAL && !allocations && !lock_attempts); no_hardware();
        CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_OPEN, 1, unknown[i]) == -EINVAL);
    }
}

static void test_raw_commands(void)
{
    static const BC_STATUS failures[] = {BC_STS_IO_ERROR, BC_STS_TIMEOUT,
        BC_STS_IO_USER_ABORT, BC_STS_CMD_CANCELLED, BC_STS_BUSY, BC_STS_PWR_MGMT,
        BC_STS_FW_CMD_ERR};
    struct crystalhd_fw_research_result result;
    struct crystalhd_fw_research_request req;
    BC_FW_CMD payload;
    unsigned selector, phase, failure, other, i, remove;

    for (selector = 0; selector <= 5; selector++)
        CHECK(!crystalhd_fw_research_raw_command(selector));
    CHECK(!crystalhd_fw_research_raw_command(12) && !crystalhd_fw_research_raw_command(UINT32_MAX));
    for (selector = 6; selector <= 11; selector++) {
        u32 commands[] = {eCMD_C011_INIT, eCMD_C011_GET_VERSION, raw_commands[selector - 6]};
        reset(); active_selector = selector; result = run();
        CHECK(!result.status && result.command_count == 3 && command_count == 3 && !result.retained);
        CHECK(download_count == 1 && release_count == 1 && firmware_release_count == 1);
        CHECK(crystalhd_fw_research_raw_command(selector) == commands[2]);
        for (i = 0; i < ARRAY_SIZE(commands); i++) {
            CHECK(result.replies[i].command == commands[i] && result.replies[i].sequence == i + 1);
            CHECK(result.replies[i].raw_response_valid && result.replies[i].header_matches);
            memset(&payload, 0xa5, sizeof(payload));
            CHECK(!crystalhd_fw_research_payload(&payload, commands[i], i + 1, selector));
            check_payload(&payload);
        }
        CHECK(!memcmp(&result.replies[3], (struct crystalhd_fw_research_reply[2]){{0}},
                      2 * sizeof(result.replies[0])));
        for (other = 0; other <= 12; other++) {
            if (other == selector) continue;
            CHECK(crystalhd_fw_research_payload(&payload, commands[2], 3, other) == -EINVAL);
        }
        CHECK(crystalhd_fw_research_payload(&payload, commands[2], 3, UINT32_MAX) == -EINVAL);
        CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_OPEN, 3, selector) == -EINVAL);
        CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_START_VIDEO, 3, selector) == -EINVAL);
        CHECK(crystalhd_fw_research_payload(&payload, 0xdeadbeef, 3, selector) == -EINVAL);
        for (phase = 0; phase < 3; phase++) {
            for (failure = 0; failure < ARRAY_SIZE(failures); failure++) {
                reset(); active_selector = selector; command_status[phase] = failures[failure]; result = run();
                CHECK(result.status == crystalhd_status_to_errno(failures[failure]));
                CHECK(result.command_count == phase + 1 && command_count == phase + 1 && release_count == 1);
                CHECK(result.replies[phase].transport_status == (u32)failures[failure]);
                CHECK(result.replies[phase].raw_response_valid == (failures[failure] == BC_STS_FW_CMD_ERR));
                CHECK(result.replies[phase].header_matches == (failures[failure] == BC_STS_FW_CMD_ERR));
                if (failures[failure] != BC_STS_FW_CMD_ERR)
                    CHECK(!memcmp(result.replies[phase].response, (u32[64]){0}, sizeof(result.replies[phase].response)));
                CHECK(!result.retained && firmware_release_count == 1);
                CHECK(!memcmp(&result.replies[phase + 1], (struct crystalhd_fw_research_reply[5]){{0}},
                              (4 - phase) * sizeof(result.replies[0])));
            }
            reset(); active_selector = selector; wrong_command = phase + 1; result = run();
            CHECK(result.status == -EPROTO && command_count == phase + 1 && release_count == 1);
            CHECK(result.replies[phase].raw_response_valid && !result.replies[phase].header_matches);
            reset(); active_selector = selector; wrong_sequence = phase + 1; result = run();
            CHECK(result.status == -EPROTO && command_count == phase + 1 && release_count == 1);
        }
        reset(); active_selector = selector; unknown_command_reply = true; result = run();
        CHECK(result.status == -EPROTO && result.command_count == 3 && command_count == 3 && release_count == 1);
        CHECK(result.replies[2].transport_status == BC_STS_SUCCESS && result.replies[2].raw_response_valid);
        CHECK(!result.replies[2].header_matches && result.replies[2].response[0] == commands[2]);
        CHECK(!memcmp(result.replies[2].response + 1, (u32[63]){0}, 63 * sizeof(u32)));
        reset(); active_selector = selector; wrong_channel = 3; result = run();
        CHECK(!result.status && result.command_count == 3 && result.replies[2].response[3] == 7);
        for (i = 0; i < 4; i++) {
            reset(); active_selector = selector;
            if (i == 0) acquire_status = BC_STS_BUSY;
            if (i == 1) { acquire_status = BC_STS_IO_ERROR; acquire_retains = true; }
            if (i == 2) download_status = BC_STS_IO_ERROR;
            if (i == 3) { release_status = BC_STS_IO_ERROR; release_retains = true; }
            result = run();
            CHECK(result.status == (i == 0 ? -EBUSY : -EIO));
            CHECK(result.retained == (i == 1 || i == 3));
            CHECK(command_count == (i == 3 ? 3U : 0U) && release_count == (i >= 2));
        }
        reset(); active_selector = selector; release_retains = true; result = run();
        CHECK(result.command_count == 3 && !result.cleanup_status && result.retained);
        reset(); active_selector = selector; unknown_command_reply = true;
        release_status = BC_STS_IO_ERROR; release_retains = true; result = run();
        CHECK(result.status == -EPROTO && result.command_count == 3 && result.retained && result.cleanup_status == BC_STS_IO_ERROR);
        reset(); active_selector = selector; command_status[2] = BC_STS_TIMEOUT;
        release_status = BC_STS_IO_ERROR; release_retains = true; result = run();
        CHECK(result.status == -ETIMEDOUT && result.retained && result.command_count == 3);
        for (remove = 1; remove <= 7; remove++) {
            reset(); active_selector = selector; remove_at = remove; result = run();
            CHECK(result.status == -ENODEV && !release_count && firmware_release_count == 1);
            CHECK(result.retained == (remove >= 3) && !result.cleanup_attempted);
            CHECK(command_count == (remove > 4 ? remove - 4 : 0));
            if (remove >= 3) CHECK(result.cleanup_status == BC_STS_CMD_CANCELLED && adp.cmds.session_module_pinned);
        }
        for (i = 0; i < 5; i++) {
            reset(); active_selector = selector; req = request();
            if (!i) req.flags = 1; else req.reserved[i - 1] = 0x70000;
            crystalhd_fw_research_run(42, &req, &result);
            CHECK(result.status == -EINVAL && !allocations && !lock_attempts); no_hardware();
        }
        reset(); active_selector = selector; bad_digest = true; result = run();
        CHECK(result.status == -EKEYREJECTED && !acquire_count && !download_count && !command_count);
        reset(); active_selector = selector; adp.cfg_users = 1; result = run();
        CHECK(result.status == -EBUSY); no_hardware();
        reset(); active_selector = selector; chd_device_generation++;
        result = run(); CHECK(result.status == -ENODEV); no_hardware();
        reset(); active_selector = selector; privileged = false;
        result = run(); CHECK(result.status == -EPERM); no_hardware();
    }
}

static void test_cleanup(void)
{
    struct crystalhd_fw_research_result result;
    unsigned remove;
    reset(); acquire_status = BC_STS_IO_ERROR; acquire_retains = true; result = run();
    CHECK(result.status == -EIO && result.retained && !result.cleanup_attempted && !release_count && !download_count);
    CHECK(adp.cmds.session_module_pinned && !adp.cmds.session_owner && adp.cmds.hw_ctx == &hardware);
    reset(); acquire_status = BC_STS_BUSY; result = run();
    CHECK(result.status == -EBUSY && !result.retained && !release_count);
    reset(); download_status = BC_STS_IO_ERROR; result = run();
    CHECK(result.status == -EIO && result.download_attempted && result.download_status == BC_STS_IO_ERROR);
    CHECK(!command_count && release_count == 1 && !result.retained);
    reset(); release_status = BC_STS_IO_ERROR; release_retains = true; result = run();
    CHECK(result.status == -EIO && result.retained && result.cleanup_attempted && release_count == 1);
    CHECK(adp.cmds.session_owner == last_owner && adp.cmds.session_module_pinned && adp.cmds.hw_ctx == &hardware);
    reset(); command_status[0] = BC_STS_TIMEOUT; release_status = BC_STS_IO_ERROR; release_retains = true; result = run();
    CHECK(result.status == -ETIMEDOUT && result.cleanup_status == BC_STS_IO_ERROR && result.retained);
    /* request, hash, acquire, download, then five commands. Removal owns the
     * terminal cleanup; this path may not invent success or drop a module pin. */
    for (remove = 1; remove <= 9; remove++) {
        reset(); remove_at = remove; result = run();
        CHECK(result.status == -ENODEV && !release_count && firmware_release_count == 1);
        CHECK(result.retained == (remove >= 3));
        CHECK(!result.cleanup_attempted);
        if (remove >= 3) CHECK(result.cleanup_status == BC_STS_CMD_CANCELLED && adp.cmds.session_module_pinned);
        CHECK(command_count == (remove > 4 ? remove - 4 : 0));
    }
}

static void test_ioctl(void)
{
    struct inode inode = {0}; struct file file = {0};
    struct crystalhd_fw_research_result result;
    unsigned long argument = (unsigned long)&result;
    unsigned malformed[] = {0, _IO('R', 0x92), _IOR('R', 0x92, struct crystalhd_fw_research_result),
        _IOWR('R', 0x91, struct crystalhd_fw_research_result),
        _IOWR('S', 0x92, struct crystalhd_fw_research_result),
        _IOWR('R', 0x92, struct crystalhd_fw_research_request)};
    unsigned i;
    reset(); CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, argument) == -ENODEV);
    CHECK(crystalhd_fw_research_open(&inode, &file) == 0); no_hardware();
    for (i = 0; i < ARRAY_SIZE(malformed); i++)
        CHECK(crystalhd_fw_research_ioctl(&file, malformed[i], argument) == -ENOTTY);
    for (i = 0; i < 9; i++) {
        result.request = request();
        if (i == 0) result.request.version++;
        else if (i == 1) result.request.size--;
        else if (i == 2) result.request.selector = 0;
        else if (i == 3) result.request.selector = 12;
        else if (i == 4) result.request.flags = 1;
        else result.request.reserved[i - 5] = 1;
        CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, argument) == -EINVAL);
    }
    no_hardware();
    copy_in_error = true; CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, argument) == -EFAULT);
    copy_in_error = false; memset(&result, 0, sizeof(result)); result.request = request();
    allocation_fail = allocations + 1;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, argument) == -ENOMEM); no_hardware();
    allocation_fail = 0;
    CHECK(crystalhd_fw_research_compat_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, argument) == 0);
    CHECK(!result.status && result.command_count == 5 && result.firmware_hash_valid);
    CHECK(crystalhd_fw_research_release(&inode, &file) == 0 && !live_allocations);
    reset(); CHECK(crystalhd_fw_research_open(&inode, &file) == 0);
    result.request = request(); copy_out_error = true;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN, argument) == -EFAULT);
    CHECK(release_count == 1 && !adp.cmds.session_owner);
    CHECK(crystalhd_fw_research_release(&inode, &file) == 0 && !live_allocations);
}

static void test_info(void)
{
    struct inode inode = {0}; struct file file = {0};
    struct crystalhd_fw_research_info info;
    unsigned long argument = (unsigned long)&info;
    unsigned i, before;
    int rc;
    reset();
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_GET_INFO, argument) == -ENODEV);
    CHECK(!lock_attempts && !copy_in_count && !copy_out_count); no_hardware();
    for (i = 0; i < 3; i++) {
        reset(); CHECK(crystalhd_fw_research_open(&inode, &file) == 0);
        if (i == 0) module.state = MODULE_STATE_COMING;
        if (i == 1) module.state = MODULE_STATE_GOING;
        if (i == 2) privileged = false;
        before = lock_attempts;
        CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_GET_INFO, argument) == (i == 2 ? -EPERM : -EAGAIN));
        CHECK(lock_attempts == before && !copy_in_count && !copy_out_count); no_hardware();
        CHECK(!crystalhd_fw_research_release(&inode, &file));
    }
    for (i = 0; i < 3; i++) {
        reset(); CHECK(crystalhd_fw_research_open(&inode, &file) == 0);
        if (i == 1) pci.device = 0x1612;
        if (i == 2) {
            adp.cfg_users = 1; adp.cmds.state = 9;
            adp.cmds.session_owner = &pci; adp.cmds.session_module_pinned = true;
            adp.cmds.hw_ctx = &hardware; adp.cmds.stream = adp.fill_byte_pool = &pci;
        }
        before = allocations; memset(&info, 0xa5, sizeof(info));
        CHECK(!crystalhd_fw_research_compat_ioctl(&file, CRYSTALHD_FW_RESEARCH_GET_INFO, argument));
        CHECK(info.version == CRYSTALHD_FW_RESEARCH_VERSION && info.size == sizeof(info) && info.generation == 42);
        CHECK(info.selector_mask == (i == 1 ? 0U : CRYSTALHD_FW_RESEARCH_SELECTOR_MASK));
        CHECK(!info.reserved[0] && !info.reserved[1] && !info.reserved[2]);
        CHECK(!memcmp(info.firmware_sha256, crystalhd_fw_research_sha256, 32));
        CHECK(allocations == (int)before && !copy_in_count && copy_out_count == 1); no_hardware();
        if (i == 2) CHECK(adp.cmds.session_owner == &pci && adp.cmds.session_module_pinned && adp.cmds.hw_ctx == &hardware);
        CHECK(!crystalhd_fw_research_release(&inode, &file));
    }
    for (i = 0; i < 6; i++) {
        reset(); CHECK(crystalhd_fw_research_open(&inode, &file) == 0);
        if (i == 0) chd_device_generation++;
        if (i == 1) adp.generation++;
        if (i == 2) adp.present = false;
        if (i == 3) g_adp_info = NULL;
        if (i == 4) adp.hw_accessible = false;
        if (i == 5) copy_out_error = true;
        rc = i == 1 ? -ESTALE : i == 4 ? -EAGAIN : i == 5 ? -EFAULT : -ENODEV;
        memset(&info, 0xa5, sizeof(info));
        CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_GET_INFO, argument) == rc);
        CHECK(info.version == 0xa5a5a5a5U && !copy_in_count && copy_out_count == (i == 5));
        no_hardware(); unlocked(); CHECK(!crystalhd_fw_research_release(&inode, &file));
    }
}

static BC_STATUS read_mock(struct crystalhd_hw *hw, u32 offset, u32 count, u32 *words)
{
    static const u32 offsets[] = {0x6fc, 0xd1ff4, 0xd3ac4, 0xd3ad0,
        0xd1ff4, 0xd3ac4, 0xd3ad0};
    static const u32 counts[] = {1, 2, 1, 1, 2, 1, 1};
    static const u32 root_offsets[] = {0x6fc, 0xd1ff4, 0xd3ac4, 0xd3ad0, 0xd3a08,
        0xd1ff4, 0xd3ac4, 0xd3ad0, 0xd3a08};
    static const u32 root_counts[] = {1, 2, 1, 1, 1, 2, 1, 1, 1};
    unsigned index = read_count++, stage = controller_only ? 2 :
        index == 0 ? 1 : index < (controller_reads ? 5U : 4U) ? 2 : 3;
    bool root_read = controller_only || (controller_reads && (index == 4 || index == 8));
    unsigned first = offset == 0xd1ff4 ? 0 : offset == 0xd3ac4 ? 2 : 3;
    unsigned i;
    u32 expected[] = {1, 0xd3a00, command_count >= 3 ? 1 : 0,
        command_count >= 3 ? 0x200 : 0};
    barrier(); CHECK(hw == &hardware && hw->lock.held == 1 && hw->fwcmd_trans_mutex.held == 1);
    CHECK(adp.cmds.session_owner == &crystalhd_fw_research_owner && adp.cmds.session_module_pinned);
    if (controller_only) CHECK(index == 0 && offset == 0xd3a08 && count == 1);
    else if (controller_reads)
        CHECK(index < ARRAY_SIZE(root_offsets) && offset == root_offsets[index] && count == root_counts[index]);
    else CHECK(index < ARRAY_SIZE(offsets) && offset == offsets[index] && count == counts[index]);
    CHECK(command_count == (stage == 3 ? 3U : 2U));
    if (read_padding) { expected[2] |= 0xa5b6c700; expected[3] |= 0xab000000; }
    for (i = 0; i < count; i++) {
        unsigned word = offset == 0x6fc ? 0 : first + i;
        words[i] = root_read ? controller_roots[index == 8] :
            offset == 0x6fc ? 0xd3a00 : expected[word];
        if (!root_read && read_mismatch_stage == stage && read_mismatch_word == word) words[i] ^= 1;
    }
    if (read_count == read_fail_at) {
        for (i = 0; i < count; i++) words[i] = 0xdeadbeef;
    }
    if (read_count == read_mutate_at) state_mutate(read_mutation);
    return read_count == read_fail_at ? read_status : BC_STS_SUCCESS;
}

static void state_reset(void)
{ reset(); firmware.size = 0xd3014; }
static struct crystalhd_fw_research_state_request state_request(void)
{
    struct crystalhd_fw_research_state_request req = {0};
    req.version = CRYSTALHD_FW_RESEARCH_VERSION;
    req.size = sizeof(struct crystalhd_fw_research_state_result);
    return req;
}
static struct crystalhd_fw_research_state_result state_run(void)
{
    struct crystalhd_fw_research_state_result result;
    struct crystalhd_fw_research_request req = request();
    memset(&result, 0xa5, sizeof(result)); result.request = state_request();
    crystalhd_fw_research_run_internal(42, &req, &result.control, &result, NULL);
    unlocked(); CHECK(!live_allocations);
    CHECK(result.control.request.selector == CRYSTALHD_FW_RESEARCH_H264_CONTROL);
    CHECK(result.control.generation == 42);
    return result;
}
static void sample_empty(const struct crystalhd_fw_research_state_sample *sample,
                         bool attempted, int status)
{
    static const u32 zero[4];
    CHECK(sample->attempted == (u32)attempted && sample->status == status);
    CHECK(!sample->read_complete && !sample->reserved && !memcmp(sample->words, zero, sizeof(zero)));
}
static struct crystalhd_fw_research_state_sample *state_stage(
    struct crystalhd_fw_research_state_result *result, unsigned stage)
{
    return stage == 1 ? &result->calibration : stage == 2 ? &result->after_init : &result->after_open;
}
static void test_fixed_state(void)
{
    static const BC_STATUS failures[] = {BC_STS_IO_ERROR, BC_STS_TIMEOUT,
        BC_STS_IO_USER_ABORT, BC_STS_CMD_CANCELLED, BC_STS_BUSY, BC_STS_PWR_MGMT,
        BC_STS_FW_CMD_ERR, BC_STS_INV_ARG};
    struct crystalhd_fw_research_state_result result;
    unsigned stage, word, read, failure, gate;
    state_reset(); result = state_run();
    CHECK(!result.control.status && command_count == 5 && read_count == 7 && release_count == 1);
    CHECK(!result.control.retained && firmware_release_count == 1);
    for (stage = 1; stage <= 3; stage++) {
        struct crystalhd_fw_research_state_sample *sample = state_stage(&result, stage);
        CHECK(sample->attempted == 1 && sample->read_complete == 1 && !sample->status && !sample->reserved);
    }
    CHECK(result.calibration.words[0] == 0xd3a00 && !result.calibration.words[1] &&
        !result.calibration.words[2] && !result.calibration.words[3]);
    CHECK(result.after_init.words[0] == 1 && result.after_init.words[1] == 0xd3a00 &&
        !result.after_init.words[2] && !result.after_init.words[3]);
    CHECK(result.after_open.words[0] == 1 && result.after_open.words[1] == 0xd3a00 &&
        result.after_open.words[2] == 1 && result.after_open.words[3] == 0x200);
    state_reset(); read_padding = true; result = state_run(); CHECK(!result.control.status && read_count == 7);
    CHECK(result.after_init.words[2] == 0xa5b6c700 && result.after_open.words[3] == 0xab000200);
    for (stage = 1; stage <= 3; stage++) {
        for (word = 0; word < (stage == 1 ? 1U : 4U); word++) {
            state_reset(); read_mismatch_stage = stage; read_mismatch_word = word; result = state_run();
            CHECK(result.control.status == -EPROTO && release_count == 1 && !result.control.retained);
            CHECK(read_count == (stage == 1 ? 1U : stage == 2 ? 4U : 7U));
            CHECK(command_count == (stage == 3 ? 3U : 2U));
            CHECK(state_stage(&result, stage)->attempted == 1 && state_stage(&result, stage)->read_complete == 1);
            CHECK(state_stage(&result, stage)->status == -EPROTO);
            for (gate = stage + 1; gate <= 3; gate++) sample_empty(state_stage(&result, gate), false, 0);
        }
    }
    for (read = 1; read <= 7; read++) {
        stage = read == 1 ? 1 : read <= 4 ? 2 : 3;
        for (failure = 0; failure < ARRAY_SIZE(failures); failure++) {
            state_reset(); read_fail_at = read; read_status = failures[failure]; result = state_run();
            CHECK(result.control.status == crystalhd_status_to_errno(failures[failure]));
            CHECK(read_count == read && command_count == (stage == 3 ? 3U : 2U));
            CHECK(release_count == 1 && !result.control.retained);
            sample_empty(state_stage(&result, stage), true, result.control.status);
            for (gate = stage + 1; gate <= 3; gate++) sample_empty(state_stage(&result, gate), false, 0);
        }
        state_reset(); read_mutate_at = read; read_mutation = 1; result = state_run();
        CHECK(result.control.status == -ENODEV && read_count == read && !release_count);
        CHECK(result.control.retained && !result.control.cleanup_attempted && result.control.cleanup_status == BC_STS_CMD_CANCELLED);
        sample_empty(state_stage(&result, stage), true, -ENODEV);
        for (gate = stage + 1; gate <= 3; gate++) sample_empty(state_stage(&result, gate), false, 0);
    }
    for (gate = 0; gate < 9; gate++) {
        state_reset();
        switch (gate) {
        case 0: adp.i2o_addr = NULL; break;
        case 1: adp.mem_addr = NULL; break;
        case 2: adp.pci_i2o_len = 0xffff; break;
        case 3: adp.pci_mem_len = 0x3ad4; break;
        case 4: adp.pci_mem_len = 0xffff; break;
        case 5: adp.generation++; break;
        case 6: privileged = false; break;
        case 7: module.state = MODULE_STATE_COMING; break;
        case 8: adp.cfg_users = 1; break;
        }
        result = state_run();
        CHECK(result.control.status == (gate < 2 ? -ENODEV : gate < 5 ? -ERANGE :
            gate == 5 ? -ESTALE : gate == 6 ? -EPERM : gate == 7 ? -EAGAIN : -EBUSY));
        no_hardware();
        for (stage = 1; stage <= 3; stage++) sample_empty(state_stage(&result, stage), false, 0);
    }
    state_reset(); firmware.size--; result = state_run(); CHECK(result.control.status == -EINVAL);
    CHECK(hash_count == 1 && !acquire_count && !download_count && !read_count && firmware_release_count == 1);
    state_reset(); bad_digest = true; result = state_run(); CHECK(result.control.status == -EKEYREJECTED);
    CHECK(!acquire_count && !read_count);
    for (gate = 0; gate < 5; gate++) {
        state_reset(); command_status[gate] = BC_STS_TIMEOUT; result = state_run();
        CHECK(result.control.status == -ETIMEDOUT && command_count == gate + 1 && release_count == 1);
        CHECK(read_count == (gate < 2 ? 0U : gate == 2 ? 4U : 7U));
    }
    state_reset(); release_status = BC_STS_IO_ERROR; release_retains = true; result = state_run();
    CHECK(result.control.status == -EIO && result.control.retained && read_count == 7);
}

static void test_fixed_state_guards(void)
{
    static const int errors[] = {0, -ENODEV, -ESTALE, -EAGAIN, -EIO,
        -EBUSY, -EBUSY, -EAGAIN, -EACCES, -ERANGE, -ENODEV};
    struct crystalhd_fw_research_state_sample sample;
    unsigned mutation, phase;
    for (phase = 0; phase < 3; phase++) {
        for (mutation = 1; mutation <= 10; mutation++) {
            /* A corrupt hw back-pointer is a preflight test only: production
             * transaction admission itself requires a valid back-pointer. */
            if (phase == 1 && mutation == 10) continue;
            state_reset(); down_read(&chd_device_lock); down_write(&adp.user_lock);
            adp.cmds.hw_ctx = &hardware; adp.cmds.session_owner = &crystalhd_fw_research_owner;
            adp.cmds.session_module_pinned = true; adp.cmds.state = BC_LINK_INIT; command_count = 2;
            if (phase == 0) state_mutate(mutation);
            if (phase == 1) transaction_mutation = mutation;
            if (phase == 2) { read_mutate_at = 1; read_mutation = mutation; }
            CHECK(crystalhd_fw_research_state_sample(&adp.cmds, 42, &sample, true, false) == errors[mutation]);
            sample_empty(&sample, phase == 2, errors[mutation]);
            CHECK(read_count == (phase == 2 ? 1U : 0U));
            up_write(&adp.user_lock); up_read(&chd_device_lock); unlocked();
        }
    }
    state_reset(); down_read(&chd_device_lock); down_write(&adp.user_lock);
    adp.cmds.hw_ctx = &hardware; adp.cmds.session_owner = &crystalhd_fw_research_owner;
    adp.cmds.session_module_pinned = true; adp.cmds.state = BC_LINK_INIT;
    transaction_error = -EINTR;
    CHECK(crystalhd_fw_research_state_sample(&adp.cmds, 42, &sample, true, false) == -ERESTARTSYS);
    sample_empty(&sample, false, -ERESTARTSYS); CHECK(!read_count);
    up_write(&adp.user_lock); up_read(&chd_device_lock); unlocked();
    for (mutation = 0; mutation < 7; mutation++) {
        int expected = mutation < 3 ? -ENODEV : mutation == 3 ? -EACCES :
            mutation == 4 ? -EBUSY : mutation == 5 ? -EOPNOTSUPP : -EACCES;
        state_reset(); down_read(&chd_device_lock); down_write(&adp.user_lock);
        adp.cmds.hw_ctx = &hardware; adp.cmds.session_owner = &crystalhd_fw_research_owner;
        adp.cmds.session_module_pinned = true; adp.cmds.state = BC_LINK_INIT;
        if (mutation == 0) adp.cmds.hw_ctx = NULL;
        if (mutation == 1) hardware.pfnDevDRAMRead = NULL;
        if (mutation == 2) hardware.pfnWriteDevRegister = NULL;
        if (mutation == 3) adp.cmds.session_module_pinned = false;
        if (mutation == 4) adp.cmds.state = BC_LINK_INVALID;
        if (mutation == 5) pci.device = 0x1612;
        if (mutation == 6) adp.cmds.session_owner = NULL;
        CHECK(crystalhd_fw_research_state_sample(&adp.cmds, 42, &sample, true, false) == expected);
        sample_empty(&sample, false, expected); CHECK(!read_count);
        up_write(&adp.user_lock); up_read(&chd_device_lock); unlocked();
    }
    state_reset();
    CHECK(!crystalhd_fw_research_state_span(&adp, 0, 1));
    CHECK(!crystalhd_fw_research_state_span(&adp, 0xfffc, 1));
    CHECK(!crystalhd_fw_research_state_span(&adp, 0x3fffffc, 1));
    CHECK(crystalhd_fw_research_state_span(&adp, 0xfffc, 2) == -ERANGE);
    CHECK(crystalhd_fw_research_state_span(&adp, 0x4000000, 1) == -ERANGE);
    CHECK(crystalhd_fw_research_state_span(&adp, 0x3fffffc, 2) == -ERANGE);
    CHECK(crystalhd_fw_research_state_span(&adp, 1, 1) == -ERANGE);
    CHECK(crystalhd_fw_research_state_span(&adp, 0, 0) == -ERANGE);
    CHECK(crystalhd_fw_research_state_span(&adp, 0, UINT32_MAX) == -ERANGE);
    adp.pci_mem_len = 0x3ad4;
    CHECK(!crystalhd_fw_research_state_span(&adp, 0xd3ad0, 1));
    adp.pci_mem_len--;
    CHECK(crystalhd_fw_research_state_span(&adp, 0xd3ad0, 1) == -ERANGE);
    adp.pci_mem_len = 3;
    CHECK(crystalhd_fw_research_state_span(&adp, 0, 1) == -ERANGE);
}

static void test_fixed_state_ioctl(void)
{
    struct inode inode = {0}; struct file file = {0};
    struct crystalhd_fw_research_state_result result;
    unsigned long arg = (unsigned long)&result;
    unsigned field;
    state_reset(); CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg) == -ENODEV);
    CHECK(!crystalhd_fw_research_open(&inode, &file));
    CHECK(crystalhd_fw_research_ioctl(&file, _IOWR('R', 0x93, struct crystalhd_fw_research_state_request), arg) == -ENOTTY);
    for (field = 0; field < 4; field++) {
        memset(&result, 0, sizeof(result)); result.request = state_request();
        if (field == 0) result.request.version++;
        if (field == 1) result.request.size--;
        if (field == 2) result.request.flags = 1;
        if (field == 3) result.request.reserved = 1;
        CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg) == -EINVAL);
    }
    no_hardware(); result.request = state_request(); copy_in_error = true;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg) == -EFAULT);
    copy_in_error = false; allocation_fail = allocations + 1;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg) == -ENOMEM); no_hardware();
    allocation_fail = 0;
    CHECK(!crystalhd_fw_research_compat_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg));
    CHECK(!result.control.status && read_count == 7 && command_count == 5 && !result.control.retained);
    CHECK(!crystalhd_fw_research_release(&inode, &file) && !live_allocations);
    state_reset(); CHECK(!crystalhd_fw_research_open(&inode, &file));
    result.request = state_request(); copy_out_error = true;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg) == -EFAULT);
    CHECK(release_count == 1 && !adp.cmds.session_owner && read_count == 7);
    CHECK(!crystalhd_fw_research_release(&inode, &file) && !live_allocations);
    state_reset(); CHECK(!crystalhd_fw_research_open(&inode, &file)); chd_device_generation++;
    result.request = state_request();
    CHECK(!crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_STATE, arg));
    CHECK(result.control.status == -ENODEV); no_hardware();
    CHECK(!crystalhd_fw_research_release(&inode, &file) && !live_allocations);
}

static void controller_reset(void)
{ state_reset(); controller_reads = true; }
static struct crystalhd_fw_research_state_request controller_request(void)
{
    struct crystalhd_fw_research_state_request req = state_request();
    req.size = sizeof(struct crystalhd_fw_research_controller_result);
    return req;
}
static struct crystalhd_fw_research_controller_result controller_run(void)
{
    struct crystalhd_fw_research_controller_result result;
    struct crystalhd_fw_research_request req = request();
    memset(&result, 0xa5, sizeof(result)); result.state.request = controller_request();
    crystalhd_fw_research_run_internal(42, &req, &result.state.control, &result.state, &result);
    unlocked(); CHECK(!live_allocations);
    CHECK(result.state.control.request.selector == CRYSTALHD_FW_RESEARCH_H264_CONTROL);
    CHECK(result.state.control.request.size == sizeof(struct crystalhd_fw_research_result));
    CHECK(result.state.control.generation == 42);
    return result;
}
static void controller_empty(const struct crystalhd_fw_research_controller_sample *sample,
                             bool attempted, int status)
{
    CHECK(sample->attempted == (u32)attempted && sample->status == status);
    CHECK(!sample->read_complete && !sample->root);
}
static void controller_complete(const struct crystalhd_fw_research_controller_sample *sample, u32 root)
{
    CHECK(sample->attempted == 1 && sample->read_complete == 1 && !sample->status && sample->root == root);
}

static void test_controller_observation_and_exact_whitelist(void)
{
    static const u32 roots[] = {0, 1, 3, 0xd5383, 0xd5384, 0xd6000, 0x115c88,
        0x116000, 0x3fffffc, 0x4000000, 0x30000f00, 0x3fffd170, UINT32_MAX};
    struct crystalhd_fw_research_controller_result result;
    unsigned i, stage;
    for (i = 0; i < ARRAY_SIZE(roots); i++) {
        controller_reset();
        controller_roots[0] = roots[i]; controller_roots[1] = roots[(i + 1) % ARRAY_SIZE(roots)];
        result = controller_run();
        CHECK(!result.state.control.status && command_count == 5 && read_count == 9 && release_count == 1);
        CHECK(!result.state.control.retained && firmware_release_count == 1);
        CHECK(result.state.request.size == sizeof(result));
        controller_complete(&result.after_init, controller_roots[0]);
        controller_complete(&result.after_open, controller_roots[1]);
        for (stage = 1; stage <= 3; stage++) {
            struct crystalhd_fw_research_state_sample *sample = state_stage(&result.state, stage);
            CHECK(sample->attempted == 1 && sample->read_complete == 1 && !sample->status && !sample->reserved);
        }
        CHECK(result.state.calibration.words[0] == 0xd3a00);
        CHECK(result.state.after_init.words[0] == 1 && result.state.after_init.words[1] == 0xd3a00 &&
            !result.state.after_init.words[2] && !result.state.after_init.words[3]);
        CHECK(result.state.after_open.words[2] == 1 && result.state.after_open.words[3] == 0x200);
        /* read_mock admits exactly nine fixed reads, independent of every
         * returned C value. Neither range, alignment nor equality is a gate.
         */
    }
    controller_reset(); read_padding = true; result = controller_run();
    CHECK(!result.state.control.status && read_count == 9);
    CHECK(result.state.after_init.words[2] == 0xa5b6c700 && result.state.after_open.words[3] == 0xab000200);
}

static void test_controller_progression_and_cleanup(void)
{
    static const BC_STATUS failures[] = {BC_STS_IO_ERROR, BC_STS_TIMEOUT,
        BC_STS_IO_USER_ABORT, BC_STS_CMD_CANCELLED, BC_STS_BUSY, BC_STS_PWR_MGMT,
        BC_STS_FW_CMD_ERR, BC_STS_INV_ARG};
    struct crystalhd_fw_research_controller_result result;
    unsigned stage, word, read, failure, gate;
    for (stage = 1; stage <= 3; stage++) {
        for (word = 0; word < (stage == 1 ? 1U : 4U); word++) {
            controller_reset(); read_mismatch_stage = stage; read_mismatch_word = word; result = controller_run();
            CHECK(result.state.control.status == -EPROTO && release_count == 1 && !result.state.control.retained);
            CHECK(read_count == (stage == 1 ? 1U : stage == 2 ? 4U : 8U));
            CHECK(command_count == (stage == 3 ? 3U : 2U));
            CHECK(state_stage(&result.state, stage)->read_complete == 1);
            CHECK(state_stage(&result.state, stage)->status == -EPROTO);
            if (stage == 3) controller_complete(&result.after_init, controller_roots[0]);
            else controller_empty(&result.after_init, false, 0);
            controller_empty(&result.after_open, false, 0);
            for (gate = stage + 1; gate <= 3; gate++) sample_empty(state_stage(&result.state, gate), false, 0);
        }
    }
    for (read = 1; read <= 9; read++) {
        stage = read == 1 ? 1 : read <= 5 ? 2 : 3;
        for (failure = 0; failure < ARRAY_SIZE(failures); failure++) {
            int error = crystalhd_status_to_errno(failures[failure]);
            controller_reset(); read_fail_at = read; read_status = failures[failure]; result = controller_run();
            CHECK(result.state.control.status == error && read_count == read);
            CHECK(command_count == (stage == 3 ? 3U : 2U) && release_count == 1 && !result.state.control.retained);
            if (read <= 4) controller_empty(&result.after_init, false, 0);
            else if (read == 5) controller_empty(&result.after_init, true, error);
            else controller_complete(&result.after_init, controller_roots[0]);
            controller_empty(&result.after_open, read == 9, read == 9 ? error : 0);
            if (read == 5 || read == 9) {
                CHECK(state_stage(&result.state, stage)->read_complete == 1);
                CHECK(!state_stage(&result.state, stage)->status);
            } else sample_empty(state_stage(&result.state, stage), true, error);
            for (gate = stage + 1; gate <= 3; gate++) sample_empty(state_stage(&result.state, gate), false, 0);
        }
        controller_reset(); read_mutate_at = read; read_mutation = 1; result = controller_run();
        CHECK(result.state.control.status == -ENODEV && read_count == read && !release_count);
        CHECK(result.state.control.retained && !result.state.control.cleanup_attempted &&
            result.state.control.cleanup_status == BC_STS_CMD_CANCELLED && firmware_release_count == 1);
        if (read <= 4) controller_empty(&result.after_init, false, 0);
        else if (read == 5) controller_empty(&result.after_init, true, -ENODEV);
        else controller_complete(&result.after_init, controller_roots[0]);
        controller_empty(&result.after_open, read == 9, read == 9 ? -ENODEV : 0);
    }
    for (gate = 0; gate < 5; gate++) {
        controller_reset(); command_status[gate] = BC_STS_TIMEOUT; result = controller_run();
        CHECK(result.state.control.status == -ETIMEDOUT && command_count == gate + 1 && release_count == 1);
        CHECK(read_count == (gate < 2 ? 0U : gate == 2 ? 5U : 9U));
        if (gate < 2) controller_empty(&result.after_init, false, 0);
        else controller_complete(&result.after_init, controller_roots[0]);
        if (gate < 3) controller_empty(&result.after_open, false, 0);
        else controller_complete(&result.after_open, controller_roots[1]);
    }
    controller_reset(); release_status = BC_STS_IO_ERROR; release_retains = true; result = controller_run();
    CHECK(result.state.control.status == -EIO && result.state.control.retained && read_count == 9);
    controller_complete(&result.after_init, controller_roots[0]);
    controller_complete(&result.after_open, controller_roots[1]);
    controller_reset(); read_fail_at = 5; release_status = BC_STS_IO_ERROR; release_retains = true;
    result = controller_run(); CHECK(result.state.control.status == -EIO && result.state.control.retained);
    CHECK(result.state.control.cleanup_status == BC_STS_IO_ERROR && read_count == 5 && command_count == 2);
    controller_empty(&result.after_init, true, -EIO); controller_empty(&result.after_open, false, 0);
    for (gate = 0; gate < 9; gate++) {
        controller_reset();
        switch (gate) {
        case 0: adp.i2o_addr = NULL; break;
        case 1: adp.mem_addr = NULL; break;
        case 2: adp.pci_i2o_len = 0xffff; break;
        case 3: adp.pci_mem_len = 0x3a0c; break;
        case 4: adp.pci_mem_len = 0xffff; break;
        case 5: adp.generation++; break;
        case 6: privileged = false; break;
        case 7: module.state = MODULE_STATE_COMING; break;
        case 8: adp.cfg_users = 1; break;
        }
        result = controller_run();
        CHECK(result.state.control.status == (gate < 2 ? -ENODEV : gate < 5 ? -ERANGE :
            gate == 5 ? -ESTALE : gate == 6 ? -EPERM : gate == 7 ? -EAGAIN : -EBUSY));
        no_hardware(); controller_empty(&result.after_init, false, 0); controller_empty(&result.after_open, false, 0);
    }
    controller_reset(); firmware.size--; result = controller_run();
    CHECK(result.state.control.status == -EINVAL && !acquire_count && !download_count && !read_count);
    controller_empty(&result.after_init, false, 0); controller_empty(&result.after_open, false, 0);
    controller_reset(); bad_digest = true; result = controller_run();
    CHECK(result.state.control.status == -EKEYREJECTED && !acquire_count && !read_count);
    controller_empty(&result.after_init, false, 0); controller_empty(&result.after_open, false, 0);
}

static void controller_guard_setup(void)
{
    state_reset(); controller_only = true;
    down_read(&chd_device_lock); down_write(&adp.user_lock);
    adp.cmds.hw_ctx = &hardware; adp.cmds.session_owner = &crystalhd_fw_research_owner;
    adp.cmds.session_module_pinned = true; adp.cmds.state = BC_LINK_INIT; command_count = 2;
}
static void controller_guard_exit(void)
{ up_write(&adp.user_lock); up_read(&chd_device_lock); unlocked(); }
static void test_controller_guarded_publication(void)
{
    static const int errors[] = {0, -ENODEV, -ESTALE, -EAGAIN, -EIO,
        -EBUSY, -EBUSY, -EAGAIN, -EACCES, -ERANGE, -ENODEV};
    struct crystalhd_fw_research_controller_sample sample;
    unsigned mutation, phase;
    for (phase = 0; phase < 3; phase++) {
        for (mutation = 1; mutation <= 10; mutation++) {
            if (phase == 1 && mutation == 10) continue;
            controller_guard_setup();
            if (phase == 0) state_mutate(mutation);
            if (phase == 1) transaction_mutation = mutation;
            if (phase == 2) { read_mutate_at = 1; read_mutation = mutation; }
            CHECK(crystalhd_fw_research_controller_sample(&adp.cmds, 42, &sample) == errors[mutation]);
            controller_empty(&sample, phase == 2, errors[mutation]);
            CHECK(read_count == (phase == 2 ? 1U : 0U)); controller_guard_exit();
        }
    }
    controller_guard_setup(); transaction_error = -EINTR;
    CHECK(crystalhd_fw_research_controller_sample(&adp.cmds, 42, &sample) == -ERESTARTSYS);
    controller_empty(&sample, false, -ERESTARTSYS); CHECK(!read_count); controller_guard_exit();
    for (mutation = 1; mutation <= 10; mutation++) {
        controller_guard_setup(); read_fail_at = read_mutate_at = 1; read_mutation = mutation;
        CHECK(crystalhd_fw_research_controller_sample(&adp.cmds, 42, &sample) == errors[mutation]);
        controller_empty(&sample, true, errors[mutation]); CHECK(read_count == 1); controller_guard_exit();
    }
    for (mutation = 0; mutation < 7; mutation++) {
        int expected = mutation < 3 ? -ENODEV : mutation == 3 ? -EACCES :
            mutation == 4 ? -EBUSY : mutation == 5 ? -EOPNOTSUPP : -EACCES;
        controller_guard_setup();
        if (mutation == 0) adp.cmds.hw_ctx = NULL;
        if (mutation == 1) hardware.pfnDevDRAMRead = NULL;
        if (mutation == 2) hardware.pfnWriteDevRegister = NULL;
        if (mutation == 3) adp.cmds.session_module_pinned = false;
        if (mutation == 4) adp.cmds.state = BC_LINK_INVALID;
        if (mutation == 5) pci.device = 0x1612;
        if (mutation == 6) adp.cmds.session_owner = NULL;
        CHECK(crystalhd_fw_research_controller_sample(&adp.cmds, 42, &sample) == expected);
        controller_empty(&sample, false, expected); CHECK(!read_count); controller_guard_exit();
    }
    controller_guard_setup(); controller_roots[0] = UINT32_MAX;
    CHECK(!crystalhd_fw_research_controller_sample(&adp.cmds, 42, &sample));
    controller_complete(&sample, UINT32_MAX); CHECK(read_count == 1); controller_guard_exit();
    state_reset(); adp.pci_mem_len = 0x3a0c;
    CHECK(!crystalhd_fw_research_state_span(&adp, 0xd3a08, 1));
    adp.pci_mem_len--;
    CHECK(crystalhd_fw_research_state_span(&adp, 0xd3a08, 1) == -ERANGE);
}

static void test_controller_stage_fences(void)
{
    static const int errors[] = {0, -ENODEV, -ESTALE, -EAGAIN, -EIO,
        -EBUSY, -EBUSY, -EAGAIN, -EACCES, -ERANGE, -ENODEV};
    struct crystalhd_fw_research_controller_result result;
    unsigned opened, mutation, phase;
    for (opened = 0; opened < 2; opened++) {
        unsigned transaction = opened ? 5 : 3, reads = opened ? 8 : 4;
        controller_reset(); transaction_error = -EINTR; transaction_error_at = transaction;
        result = controller_run();
        CHECK(result.state.control.status == -ERESTARTSYS && read_count == reads);
        CHECK(command_count == (opened ? 3U : 2U) && release_count == 1 && !result.state.control.retained);
        if (opened) controller_complete(&result.after_init, controller_roots[0]);
        else controller_empty(&result.after_open, false, 0);
        controller_empty(opened ? &result.after_open : &result.after_init, false, -ERESTARTSYS);
        CHECK(state_stage(&result.state, opened ? 3 : 2)->read_complete == 1);
        if (!opened) sample_empty(&result.state.after_open, false, 0);
        for (phase = 0; phase < 2; phase++) {
            for (mutation = 1; mutation <= 10; mutation++) {
                /* Replacing the owner is tested directly above; the session
                 * release mock deliberately requires the original owner.
                 */
                if (mutation == 8 || (phase == 0 && mutation == 10)) continue;
                controller_reset();
                if (phase == 0) {
                    transaction_mutation = mutation; transaction_mutation_at = transaction;
                } else { read_mutate_at = reads + 1; read_mutation = mutation; }
                result = controller_run();
                CHECK(result.state.control.status == errors[mutation]);
                CHECK(read_count == reads + phase && command_count == (opened ? 3U : 2U));
                CHECK(release_count == (mutation == 1 ? 0 : 1));
                CHECK(result.state.control.retained == (u32)(mutation == 1));
                if (opened) controller_complete(&result.after_init, controller_roots[0]);
                else controller_empty(&result.after_open, false, 0);
                controller_empty(opened ? &result.after_open : &result.after_init, phase == 1, errors[mutation]);
                CHECK(state_stage(&result.state, opened ? 3 : 2)->read_complete == 1);
                if (!opened) sample_empty(&result.state.after_open, false, 0);
            }
        }
    }
}

static void test_controller_ioctl_and_compat(void)
{
    struct inode inode = {0}; struct file file = {0};
    struct crystalhd_fw_research_controller_result result;
    unsigned long arg = (unsigned long)&result;
    const unsigned malformed[] = {_IO('R', 0x94),
        _IOR('R', 0x94, struct crystalhd_fw_research_controller_result),
        _IOWR('R', 0x94, struct crystalhd_fw_research_state_result),
        _IOWR('R', 0x94, struct crystalhd_fw_research_state_request),
        _IOWR('S', 0x94, struct crystalhd_fw_research_controller_result)};
    unsigned field;
    controller_reset(); CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg) == -ENODEV);
    CHECK(!crystalhd_fw_research_open(&inode, &file));
    for (field = 0; field < ARRAY_SIZE(malformed); field++)
        CHECK(crystalhd_fw_research_ioctl(&file, malformed[field], arg) == -ENOTTY);
    for (field = 0; field < 6; field++) {
        memset(&result, 0, sizeof(result)); result.state.request = controller_request();
        if (field == 0) result.state.request.version++;
        if (field == 1) result.state.request.size--;
        if (field == 2) result.state.request.flags = 1;
        if (field == 3) result.state.request.reserved = 1;
        if (field == 4) result.state.request.size = sizeof(struct crystalhd_fw_research_state_result);
        if (field == 5) result.state.request.size = UINT32_MAX;
        CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg) == -EINVAL);
    }
    no_hardware(); result.state.request = controller_request(); copy_in_error = true;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg) == -EFAULT);
    copy_in_error = false; allocation_fail = allocations + 1;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg) == -ENOMEM); no_hardware();
    allocation_fail = 0;
    CHECK(!crystalhd_fw_research_compat_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg));
    CHECK(!result.state.control.status && read_count == 9 && command_count == 5 && !result.state.control.retained);
    CHECK(result.state.request.size == sizeof(result) && result.state.control.request.size == 1488);
    controller_complete(&result.after_init, controller_roots[0]); controller_complete(&result.after_open, controller_roots[1]);
    CHECK(!crystalhd_fw_research_release(&inode, &file) && !live_allocations);
    controller_reset(); CHECK(!crystalhd_fw_research_open(&inode, &file));
    result.state.request = controller_request(); copy_out_error = true;
    CHECK(crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg) == -EFAULT);
    CHECK(release_count == 1 && !adp.cmds.session_owner && read_count == 9);
    CHECK(!crystalhd_fw_research_release(&inode, &file) && !live_allocations);
    controller_reset(); CHECK(!crystalhd_fw_research_open(&inode, &file)); chd_device_generation++;
    result.state.request = controller_request();
    CHECK(!crystalhd_fw_research_ioctl(&file, CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER, arg));
    CHECK(result.state.control.status == -ENODEV); no_hardware();
    controller_empty(&result.after_init, false, 0); controller_empty(&result.after_open, false, 0);
    CHECK(!crystalhd_fw_research_release(&inode, &file) && !live_allocations);
}

int main(void)
{
    _Static_assert(sizeof(struct crystalhd_fw_research_info) == 64, "info ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_info, generation) == 8, "info generation ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_info, selector_mask) == 16, "info mask ABI");
    _Static_assert(CRYSTALHD_FW_RESEARCH_GET_INFO == 0x80405291U, "info ioctl ABI");
    _Static_assert(sizeof(struct crystalhd_fw_research_request) == 32, "request ABI");
    _Static_assert(sizeof(struct crystalhd_fw_research_reply) == 276, "reply ABI");
    _Static_assert(sizeof(struct crystalhd_fw_research_result) == 1488, "result ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_result, generation) == 32, "generation ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_result, replies) == 104, "reply offset ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_result, firmware_hash_valid) == 68, "hash validity ABI");
    _Static_assert(_IOC_SIZE(CRYSTALHD_FW_RESEARCH_RUN) == 1488, "ioctl ABI");
    _Static_assert(sizeof(struct crystalhd_fw_research_state_result) == 1600, "unchanged state ABI");
    _Static_assert(CRYSTALHD_FW_RESEARCH_RUN_STATE == 0xc6405293U, "unchanged state ioctl");
    _Static_assert(sizeof(struct crystalhd_fw_research_controller_sample) == 16, "controller sample ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_controller_sample, root) == 12, "controller root ABI");
    _Static_assert(sizeof(struct crystalhd_fw_research_controller_result) == 1632, "controller result ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_controller_result, state) == 0, "controller state ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_controller_result, after_init) == 1600, "controller init ABI");
    _Static_assert(offsetof(struct crystalhd_fw_research_controller_result, after_open) == 1616, "controller open ABI");
    _Static_assert(CRYSTALHD_FW_RESEARCH_RUN_CONTROLLER == 0xc6605294U, "controller ioctl ABI");
    _Static_assert(CRYSTALHD_FW_RESEARCH_H261_CONTROL == 3 && CRYSTALHD_FW_RESEARCH_H263_CONTROL == 4 &&
                   CRYSTALHD_FW_RESEARCH_MPEG1_CONTROL == 5 && CRYSTALHD_FW_RESEARCH_SELECTOR_MASK == 2047, "named selectors");
    _Static_assert(CRYSTALHD_FW_RESEARCH_SCALING_FILTERS_COMMAND == 6 &&
                   CRYSTALHD_FW_RESEARCH_PIC_CAPTURE_COMMAND == 7 && CRYSTALHD_FW_RESEARCH_SET_CSC_COMMAND == 8 &&
                   CRYSTALHD_FW_RESEARCH_SET_FGT_COMMAND == 9 && CRYSTALHD_FW_RESEARCH_CUSTOM_VIDOUT_COMMAND == 10 &&
                   CRYSTALHD_FW_RESEARCH_FILL_PIC_BUF_COMMAND == 11, "fixed command selectors");
    test_lifecycle(); test_admission(); test_idle(); test_hash(); test_commands(); test_named_controls(); test_raw_commands(); test_cleanup(); test_ioctl(); test_info();
    test_fixed_state(); test_fixed_state_guards(); test_fixed_state_ioctl();
    test_controller_observation_and_exact_whitelist(); test_controller_progression_and_cleanup();
    test_controller_guarded_publication(); test_controller_stage_fences(); test_controller_ioctl_and_compat();
    CHECK(!live_allocations); unlocked();
    printf("Firmware probe: %u checks passed\n", checks);
    return 0;
}
