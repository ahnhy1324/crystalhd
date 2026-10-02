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

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { u32 cmd[64], rsp[64], flags, add_data; } BC_FW_CMD;
enum { BC_LINK_INVALID = 0, DTS_MODE_INV = -1, BC_PCI_DEVID_FLEA = 0x1615 };
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
struct crystalhd_cmd {
    struct crystalhd_adp *adp;
    const void *session_owner, *session_lifetime_owner, *session_lifetime_ops;
    bool session_module_pinned, retain_rx_on_suspend;
    void *stream, *hw_ctx;
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
static int remove_at, step;
static const void *last_owner;
static unsigned lifecycle[16], lifecycle_count;
static int v4l2_error, misc_error, pci_error;
static int bc_chd_driver;
static unsigned lock_attempts;
static unsigned metadata_race;

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
{ CHECK(!chd_device_lock.readers && !adp.user_lock.writers && !adp.user_lock.readers); }
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
    ctx->hw_ctx = ctx->stream = NULL;
}
static BC_STATUS crystalhd_session_acquire_locked(struct crystalhd_cmd *ctx, const void *owner)
{
    barrier(); CHECK(ctx == &adp.cmds && owner && hash_count == 1 && !bad_digest && !digest_error);
    acquire_count++; last_owner = owner; advance();
    if (acquire_status == BC_STS_SUCCESS) {
        ctx->session_owner = owner; ctx->session_module_pinned = true;
        ctx->hw_ctx = &pci; ctx->fw_sequence = 0;
    } else if (acquire_retains) {
        ctx->session_module_pinned = true; ctx->hw_ctx = &pci;
    }
    return acquire_status;
}
static BC_STATUS crystalhd_fw_download_locked(struct crystalhd_cmd *ctx, const void *owner,
                                             const void *bytes, size_t size)
{
    barrier(); CHECK(ctx == &adp.cmds && owner == last_owner && ctx->session_owner == owner);
    CHECK(bytes == firmware.data && size == firmware.size && hash_count == 1);
    CHECK(download_count++ == 0); advance();
    return download_status;
}
static void check_payload(const BC_FW_CMD *cmd);
static BC_STATUS crystalhd_fw_exec_locked(struct crystalhd_cmd *ctx, const void *owner, BC_FW_CMD *cmd)
{
    unsigned index = command_count++;
    barrier(); CHECK(index < 5 && ctx == &adp.cmds && owner == last_owner && ctx->session_owner == owner);
    check_payload(cmd);
    CHECK(cmd->cmd[1] == index + 1);
    cmd->rsp[0] = cmd->cmd[0] ^ (index + 1 == wrong_command ? 1U : 0U);
    cmd->rsp[1] = cmd->cmd[1] + (index + 1 == wrong_sequence ? 1U : 0U);
    cmd->rsp[2] = command_status[index] == BC_STS_FW_CMD_ERR ? 0x1234 : 0;
    cmd->rsp[3] = index + 1 == wrong_channel ? 7 : 0;
    cmd->rsp[63] = 0xfeed;
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
    case eCMD_C011_GET_VERSION: case eCMD_C011_DEC_CHAN_STATUS: break;
    case eCMD_C011_DEC_CHAN_OPEN: expected[4] = 1; break;
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
    req.selector = CRYSTALHD_FW_RESEARCH_H264_CONTROL;
    return req;
}
static void reset(void)
{
    unsigned i;
    CHECK(!live_allocations); unlocked();
    memset(&adp, 0, sizeof(adp)); memset(&pci, 0, sizeof(pci));
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
{ CHECK(!request_count && !hash_count && !acquire_count && !download_count && !command_count && !release_count); }

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
        case 12: adp.cmds.hw_ctx = &pci; break;
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
        CHECK(!crystalhd_fw_research_payload(&payload, commands[i], i + 1)); check_payload(&payload);
    }
    CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_START_VIDEO, 1) == -EINVAL);
    CHECK(crystalhd_fw_research_payload(&payload, eCMD_C011_DEC_CHAN_PIC_CAPTURE, 1) == -EINVAL);
    CHECK(crystalhd_fw_research_payload(&payload, 0xdeadbeef, 1) == -EINVAL);
    reset(); req = request(); req.selector = CRYSTALHD_FW_RESEARCH_VERSION_ONLY;
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
    reset(); adp.cmds.fw_sequence = 5; memset(&result, 0, sizeof(result));
    CHECK(crystalhd_fw_research_command(&adp.cmds, &result, eCMD_C011_INIT) == -EOVERFLOW && !command_count);
    adp.cmds.fw_sequence = 0; result.command_count = 5;
    CHECK(crystalhd_fw_research_command(&adp.cmds, &result, eCMD_C011_INIT) == -EOVERFLOW && !command_count);
}

static void test_cleanup(void)
{
    struct crystalhd_fw_research_result result;
    unsigned remove;
    reset(); acquire_status = BC_STS_IO_ERROR; acquire_retains = true; result = run();
    CHECK(result.status == -EIO && result.retained && !result.cleanup_attempted && !release_count && !download_count);
    CHECK(adp.cmds.session_module_pinned && !adp.cmds.session_owner && adp.cmds.hw_ctx == &pci);
    reset(); acquire_status = BC_STS_BUSY; result = run();
    CHECK(result.status == -EBUSY && !result.retained && !release_count);
    reset(); download_status = BC_STS_IO_ERROR; result = run();
    CHECK(result.status == -EIO && result.download_attempted && result.download_status == BC_STS_IO_ERROR);
    CHECK(!command_count && release_count == 1 && !result.retained);
    reset(); release_status = BC_STS_IO_ERROR; release_retains = true; result = run();
    CHECK(result.status == -EIO && result.retained && result.cleanup_attempted && release_count == 1);
    CHECK(adp.cmds.session_owner == last_owner && adp.cmds.session_module_pinned && adp.cmds.hw_ctx == &pci);
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
        else if (i == 3) result.request.selector = 3;
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
            adp.cmds.hw_ctx = adp.cmds.stream = adp.fill_byte_pool = &pci;
        }
        before = allocations; memset(&info, 0xa5, sizeof(info));
        CHECK(!crystalhd_fw_research_compat_ioctl(&file, CRYSTALHD_FW_RESEARCH_GET_INFO, argument));
        CHECK(info.version == CRYSTALHD_FW_RESEARCH_VERSION && info.size == sizeof(info) && info.generation == 42);
        CHECK(info.selector_mask == (i == 1 ? 0U : CRYSTALHD_FW_RESEARCH_SELECTOR_MASK));
        CHECK(!info.reserved[0] && !info.reserved[1] && !info.reserved[2]);
        CHECK(!memcmp(info.firmware_sha256, crystalhd_fw_research_sha256, 32));
        CHECK(allocations == (int)before && !copy_in_count && copy_out_count == 1); no_hardware();
        if (i == 2) CHECK(adp.cmds.session_owner == &pci && adp.cmds.session_module_pinned && adp.cmds.hw_ctx == &pci);
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
    test_lifecycle(); test_admission(); test_idle(); test_hash(); test_commands(); test_cleanup(); test_ioctl(); test_info();
    CHECK(!live_allocations); unlocked();
    printf("Firmware probe: %u checks passed\n", checks);
    return 0;
}
