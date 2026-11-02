// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROBE_MARKER "CRYSTALHD_KODI_EMBEDDED_PROBE"
#define PROBE_FD "CRYSTALHD_KODI_EMBEDDED_PROBE_FD"
#define PROBE_DECODER "CRYSTALHD_KODI_EMBEDDED_PROBE_DECODER"
#define PROBE_AVCODEC_MAJOR "CRYSTALHD_KODI_EMBEDDED_PROBE_AVCODEC_MAJOR"
#define PROBE_VERSION "v1"
#define CONFIG_LIMIT 64

struct ffmpeg_hw_config {
	int pix_fmt;
	int methods;
	int device_type;
};

struct main_image {
	ElfW(Addr) load_bias;
	const ElfW(Phdr) *headers;
	ElfW(Half) header_count;
	bool found;
};

typedef unsigned int (*version_function)(void);
typedef const void *(*find_decoder_function)(const char *name);
typedef const struct ffmpeg_hw_config *(*get_hw_config_function)(const void *codec,
								 int index);
typedef const char *(*name_function)(int value);
typedef int (*kodi_version_function)(void);
typedef int (*libc_start_main_function)(int (*main_function)(int, char **, char **),
					       int argc, char **argv,
					       void (*init_function)(void),
					       void (*fini_function)(void),
					       void (*rtld_fini_function)(void),
					       void *stack_end);

static int output_fd = -1;
static bool protocol_started;
static bool probe_is_active;

static bool activation_requested(void)
{
	const char *marker = getenv(PROBE_MARKER);

	return marker != NULL && strcmp(marker, PROBE_VERSION) == 0;
}

static void exit_without_protocol(void)
{
	_exit(120);
}

static void write_all(const char *value, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t written = write(output_fd, value + offset, length - offset);

		if (written > 0) {
			offset += (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		exit_without_protocol();
	}
}

static void emit_line(const char *format, ...)
{
	char buffer[512];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(buffer, sizeof(buffer), format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(buffer))
		exit_without_protocol();
	write_all(buffer, (size_t)length);
}

static bool safe_token(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;

	if (value == NULL || *value == '\0')
		return false;
	for (; *cursor != '\0'; cursor++) {
		if ((*cursor >= 'a' && *cursor <= 'z') ||
		    (*cursor >= 'A' && *cursor <= 'Z') ||
		    (*cursor >= '0' && *cursor <= '9') ||
		    *cursor == '_' || *cursor == '.' || *cursor == '-')
			continue;
		return false;
	}
	return true;
}

static void start_protocol(void)
{
	if (protocol_started)
		return;
	emit_line("crystalhd-kodi-embedded-ffmpeg-hwconfig\t1\n");
	protocol_started = true;
}

__attribute__((noreturn))
static void probe_error(const char *code, const char *detail)
{
	if (!safe_token(code) || !safe_token(detail))
		exit_without_protocol();
	start_protocol();
	emit_line("error\t%s\t%s\n", code, detail);
	_exit(120);
}

static int parse_output_fd(void)
{
	const char *value = getenv(PROBE_FD);
	unsigned long parsed = 0;
	const unsigned char *cursor;

	if (value == NULL || *value == '\0')
		return -1;
	for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return -1;
		if (parsed > ((unsigned long)INT_MAX - (unsigned long)(*cursor - '0')) / 10UL)
			return -1;
		parsed = parsed * 10UL + (unsigned long)(*cursor - '0');
	}
	if (parsed < 3UL)
		return -1;
	return (int)parsed;
}

static int parse_expected_avcodec_major(void)
{
	const char *value = getenv(PROBE_AVCODEC_MAJOR);
	unsigned int parsed = 0;
	const unsigned char *cursor;

	if (value == NULL || *value == '\0')
		return -1;
	for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			probe_error("invalid_expected_abi", "avcodec_major");
		parsed = parsed * 10U + (unsigned int)(*cursor - '0');
		if (parsed > 255U)
			probe_error("invalid_expected_abi", "avcodec_major");
	}
	return (int)parsed;
}

static int capture_main_image(struct dl_phdr_info *information, size_t size, void *data)
{
	struct main_image *image = data;

	(void)size;
	if (image->found)
		return 1;
	/* The main executable is the first object and conventionally has an empty name. */
	if (information->dlpi_name != NULL && information->dlpi_name[0] != '\0')
		return 0;
	image->load_bias = information->dlpi_addr;
	image->headers = information->dlpi_phdr;
	image->header_count = information->dlpi_phnum;
	image->found = true;
	return 1;
}

static bool address_in_main_image(const struct main_image *image, const void *address)
{
	uintptr_t candidate = (uintptr_t)address;
	ElfW(Half) index;

	for (index = 0; index < image->header_count; index++) {
		const ElfW(Phdr) *header = &image->headers[index];
		uintptr_t start;
		uintptr_t end;

		if (header->p_type != PT_LOAD || header->p_memsz == 0)
			continue;
		start = (uintptr_t)image->load_bias + (uintptr_t)header->p_vaddr;
		if ((uintptr_t)header->p_memsz > UINTPTR_MAX - start)
			return false;
		end = start + (uintptr_t)header->p_memsz;
		if (candidate >= start && candidate < end)
			return true;
	}
	return false;
}

static const char *base_name(const char *path)
{
	const char *slash;

	if (path == NULL)
		return "";
	slash = strrchr(path, '/');
	return slash == NULL ? path : slash + 1;
}

static bool core_libav_name(const char *name, const char **kind)
{
	static const char *const names[] = { "libavcodec.so", "libavformat.so", "libavutil.so" };
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		size_t length = strlen(names[index]);

		if (strncmp(name, names[index], length) != 0)
			continue;
		if (name[length] != '\0' && name[length] != '.')
			continue;
		*kind = index == 0 ? "libavcodec" : (index == 1 ? "libavformat" : "libavutil");
		return true;
	}
	return false;
}

static int reject_core_libav(struct dl_phdr_info *information, size_t size, void *data)
{
	const char **kind = data;

	(void)size;
	if (core_libav_name(base_name(information->dlpi_name), kind))
		return 1;
	return 0;
}

static void *lookup_symbol(const struct main_image *image, const char *name, bool required,
			   bool *present)
{
	Dl_info information;
	const char *error;
	void *symbol;

	dlerror();
	symbol = dlsym(RTLD_DEFAULT, name);
	error = dlerror();
	if (error != NULL || symbol == NULL) {
		*present = false;
		if (required)
			probe_error("missing_symbol", name);
		return NULL;
	}
	*present = true;
	if (dladdr(symbol, &information) == 0 || information.dli_fbase == NULL)
		probe_error("symbol_dladdr_failed", name);
	if (!address_in_main_image(image, symbol))
		probe_error("symbol_wrong_origin", name);
	return symbol;
}

static void copy_function_pointer(void *destination, size_t destination_size, void *symbol)
{
	if (destination_size != sizeof(symbol))
		probe_error("function_pointer_size", "unsupported_architecture");
	memcpy(destination, &symbol, destination_size);
}

static const char *validated_ffmpeg_name(const char *value, const char *kind)
{
	if (value == NULL)
		return "-";
	if (!safe_token(value))
		probe_error("invalid_ffmpeg_name", kind);
	return value;
}

static void emit_kodi_version(const struct main_image *image)
{
	static const char major_name[] = "_ZN12CCompileInfo8GetMajorEv";
	static const char minor_name[] = "_ZN12CCompileInfo8GetMinorEv";
	bool major_present;
	bool minor_present;
	void *major_symbol = lookup_symbol(image, major_name, false, &major_present);
	void *minor_symbol = lookup_symbol(image, minor_name, false, &minor_present);
	kodi_version_function major_function;
	kodi_version_function minor_function;
	int major;
	int minor;

	if (!major_present && !minor_present) {
		emit_line("kodi_version\tunavailable\t-1\t-1\n");
		return;
	}
	if (!major_present || !minor_present)
		probe_error("incomplete_kodi_version", "CCompileInfo");
	copy_function_pointer(&major_function, sizeof(major_function), major_symbol);
	copy_function_pointer(&minor_function, sizeof(minor_function), minor_symbol);
	major = major_function();
	minor = minor_function();
	if (major < 0 || major > 999 || minor < 0 || minor > 999)
		probe_error("invalid_kodi_version", "CCompileInfo");
	emit_line("kodi_version\tavailable\t%d\t%d\n", major, minor);
}

__attribute__((noreturn))
static void run_probe(void)
{
	struct main_image image = { 0 };
	const char *loaded_kind = NULL;
	const char *decoder_name = getenv(PROBE_DECODER);
	bool present;
	void *symbol;
	version_function avcodec_version_function;
	version_function avformat_version_function;
	version_function avutil_version_function;
	find_decoder_function find_decoder;
	get_hw_config_function get_hw_config;
	name_function pixel_format_name;
	name_function device_type_name;
	unsigned int avcodec_version;
	unsigned int avformat_version;
	unsigned int avutil_version;
	unsigned int avcodec_major;
	int expected_avcodec_major;
	bool avcodec_version_present;
	bool avformat_version_present;
	bool avutil_version_present;
	const void *codec;
	int index;

	if (!safe_token(decoder_name))
		probe_error("invalid_decoder", "environment");
	if (dl_iterate_phdr(capture_main_image, &image) == 0 || !image.found)
		probe_error("main_image_missing", "dl_iterate_phdr");
	if (dl_iterate_phdr(reject_core_libav, &loaded_kind) != 0)
		probe_error("dynamic_libav_loaded", loaded_kind);

	symbol = lookup_symbol(&image, "avcodec_version", false, &avcodec_version_present);
	if (avcodec_version_present)
		copy_function_pointer(&avcodec_version_function, sizeof(avcodec_version_function), symbol);
	symbol = lookup_symbol(&image, "avformat_version", false, &avformat_version_present);
	if (avformat_version_present)
		copy_function_pointer(&avformat_version_function, sizeof(avformat_version_function), symbol);
	symbol = lookup_symbol(&image, "avutil_version", false, &avutil_version_present);
	if (avutil_version_present)
		copy_function_pointer(&avutil_version_function, sizeof(avutil_version_function), symbol);
	symbol = lookup_symbol(&image, "avcodec_find_decoder_by_name", true, &present);
	copy_function_pointer(&find_decoder, sizeof(find_decoder), symbol);
	symbol = lookup_symbol(&image, "avcodec_get_hw_config", true, &present);
	copy_function_pointer(&get_hw_config, sizeof(get_hw_config), symbol);
	symbol = lookup_symbol(&image, "av_get_pix_fmt_name", true, &present);
	copy_function_pointer(&pixel_format_name, sizeof(pixel_format_name), symbol);
	symbol = lookup_symbol(&image, "av_hwdevice_get_type_name", true, &present);
	copy_function_pointer(&device_type_name, sizeof(device_type_name), symbol);

	expected_avcodec_major = parse_expected_avcodec_major();
	if (avcodec_version_present) {
		avcodec_version = avcodec_version_function();
		avcodec_major = (avcodec_version >> 16) & 0xffU;
	} else {
		if (expected_avcodec_major < 0)
			probe_error("missing_avcodec_abi", "no_version_or_attestation");
		avcodec_version = UINT_MAX;
		avcodec_major = (unsigned int)expected_avcodec_major;
	}
	if (avcodec_major < 58U || avcodec_major > 63U) {
		char detail[32];

		(void)snprintf(detail, sizeof(detail), "major_%u", avcodec_major);
		probe_error("unsupported_avcodec_abi", detail);
	}
	if (expected_avcodec_major >= 0 && avcodec_major != (unsigned int)expected_avcodec_major)
		probe_error("avcodec_abi_mismatch", "version_vs_attestation");
	avformat_version = avformat_version_present ? avformat_version_function() : UINT_MAX;
	avutil_version = avutil_version_present ? avutil_version_function() : UINT_MAX;
	emit_line("version\tavcodec\t%s\n",
		  avcodec_version_present ? "available" : "unavailable");
	if (avcodec_version_present)
		emit_line("version_value\tavcodec\t%u\n", avcodec_version);
	emit_line("version\tavformat\t%s\n",
		  avformat_version_present ? "available" : "unavailable");
	if (avformat_version_present)
		emit_line("version_value\tavformat\t%u\n", avformat_version);
	emit_line("version\tavutil\t%s\n", avutil_version_present ? "available" : "unavailable");
	if (avutil_version_present)
		emit_line("version_value\tavutil\t%u\n", avutil_version);
	emit_line("avcodec_abi\t%u\t%s\n", avcodec_major,
		  avcodec_version_present ? "version_export" : "known_package_identity");
	emit_kodi_version(&image);

	codec = find_decoder(decoder_name);
	if (codec == NULL) {
		emit_line("decoder\t0\n");
		emit_line("done\n");
		_exit(0);
	}
	emit_line("decoder\t1\n");
	for (index = 0; index <= CONFIG_LIMIT; index++) {
		const struct ffmpeg_hw_config *config = get_hw_config(codec, index);
		const char *pixel_name;
		const char *device_name;

		if (config == NULL)
			break;
		if (index == CONFIG_LIMIT)
			probe_error("unterminated_config", "limit_64");
		pixel_name = validated_ffmpeg_name(pixel_format_name(config->pix_fmt), "pixel_format");
		device_name = validated_ffmpeg_name(device_type_name(config->device_type), "device_type");
		emit_line("config\t%d\t%d\t%s\t%d\t%d\t%s\n", index, config->pix_fmt,
			  pixel_name, config->methods, config->device_type, device_name);
	}
	emit_line("done\n");
	_exit(0);
}

__attribute__((constructor(101)))
static void embedded_probe_constructor(void)
{
	if (!activation_requested())
		return;
	probe_is_active = true;
	output_fd = parse_output_fd();
	if (output_fd < 0)
		exit_without_protocol();
	start_protocol();
	run_probe();
}

__attribute__((visibility("default")))
int __libc_start_main(int (*main_function)(int, char **, char **), int argc, char **argv,
		      void (*init_function)(void), void (*fini_function)(void),
		      void (*rtld_fini_function)(void), void *stack_end)
{
	libc_start_main_function real_start;
	void *symbol;
	const char *error;

	if (probe_is_active || activation_requested()) {
		if (output_fd < 0)
			output_fd = parse_output_fd();
		if (output_fd >= 0)
			probe_error("constructor_returned", "main_blocked");
		exit_without_protocol();
	}
	dlerror();
	symbol = dlsym(RTLD_NEXT, "__libc_start_main");
	error = dlerror();
	if (error != NULL || symbol == NULL)
		exit_without_protocol();
	copy_function_pointer(&real_start, sizeof(real_start), symbol);
	return real_start(main_function, argc, argv, init_function, fini_function,
			  rtld_fini_function, stack_end);
}
