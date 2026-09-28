#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Phase 1 manual hardware release gates.  Long-running modes intentionally
# require an external fixture oracle and an unused decoder.

set -eu
umask 027
LC_ALL=C
export LC_ALL

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
plugin_dir=$repo_dir/filters/gst/gst-plugin-1.0

release_environment_overridden=no
[ "${PHASE1_FFPROBE+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_SHA256SUM+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_TIMEOUT_COMMAND+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_FUSER+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_MODULES_FILE+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_DEVICE+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_SAMPLE_INTERVAL+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_RSS_GROWTH_KB+x}" != x ] || release_environment_overridden=yes
[ "${PHASE1_LOCK_FILE+x}" != x ] || release_environment_overridden=yes

FFPROBE=${PHASE1_FFPROBE:-ffprobe}
SHA256SUM=${PHASE1_SHA256SUM:-sha256sum}
if [ -x /usr/bin/gnutimeout ]; then
    default_timeout=/usr/bin/gnutimeout
else
    default_timeout=timeout
fi
TIMEOUT=${PHASE1_TIMEOUT_COMMAND:-$default_timeout}
FUSER=${PHASE1_FUSER:-fuser}
MODULES_FILE=${PHASE1_MODULES_FILE:-/proc/modules}
DEVICE=${PHASE1_DEVICE:-/dev/crystalhd}
SAMPLE_INTERVAL=${PHASE1_SAMPLE_INTERVAL:-1}
RSS_GROWTH_KB=${PHASE1_RSS_GROWTH_KB:-32768}
# The monitor records ten rapid loader/initialization samples.  Keep them in
# the evidence, but require six later interval samples for a growth trend.
RESOURCE_STARTUP_SAMPLES=10
RESOURCE_TREND_SAMPLES=6
PROC_ROOT=/proc

results=
runtime=
mode=
initialized=no
finished=no
reload_in_progress=no
reload_force_value=0
started_at=
revision=
registry=
original_args=$*
stage_number=0
active_watchdog=
active_monitor=
rc=no
acceptance=development

say()
{
    printf '%s\n' "$*"
}

error()
{
    printf 'phase1-release-gate: %s\n' "$*" >&2
    if [ "$initialized" = yes ] && [ -d "$results" ]; then
        printf 'phase1-release-gate: %s\n' "$*" >> "$results/errors.log"
    fi
}

die()
{
    error "$*"
    exit 1
}

watchdog_result_valid()
{
    [ "$1" -eq 137 ] && [ "$2" -ge 1 ] && [ "$2" -le 4 ]
}

watchdog_works()
{
    watchdog_command=$1
    watchdog_started=$(date +%s) || return 1
    watchdog_status=0
    "$watchdog_command" --signal=TERM --kill-after=1 1 \
        sh -c 'trap "" TERM; exec sleep 10' >/dev/null 2>&1 || \
        watchdog_status=$?
    watchdog_finished=$(date +%s) || return 1
    watchdog_elapsed=$((watchdog_finished - watchdog_started))
    watchdog_result_valid "$watchdog_status" "$watchdog_elapsed"
}

usage()
{
    cat >&2 <<EOF
usage:
  $0 --self-test
  $0 manifest-template
  $0 manifest-check FILE
  $0 evidence-check EVIDENCE_ROOT
  $0 soak [--rc] --runtime source|installed --results DIR --fixture FILE \\
     --input-sha256 HEX --codec h264 --profile PROFILE --geometry WIDTHxHEIGHT \\
     --field FIELD --frames N --seconds N [--timeout N] [--video-only]
  $0 churn [--rc] --runtime source|installed --results DIR --fixture FILE \\
     --input-sha256 HEX --codec CODEC --profile PROFILE --geometry WIDTHxHEIGHT \\
     --field FIELD --frames N [--iterations N] [--timeout N]
  $0 oracle --runtime source|installed --results DIR --manifest FILE \\
     --fixture-root DIR [--timeout N]
  $0 round-robin [--rc] --runtime source|installed --results DIR --manifest FILE \\
     --fixture-root DIR [--cycles N] [--timeout N]
  $0 controls [--rc] --runtime source|installed --results DIR --fixture FILE \\
     --input-sha256 HEX --codec h264 --profile PROFILE --geometry WIDTHxHEIGHT \\
     --field FIELD --frames 360 [--cycles N] [--timeout N] [--video-only] [--skip-2x]
  $0 reload-sanity [--rc] --runtime source|installed --results DIR --fixture FILE \\
     --input-sha256 HEX --codec CODEC --profile PROFILE --geometry WIDTHxHEIGHT \\
     --field FIELD --frames N [--timeout N]

Every hardware invocation requires a new absolute results directory outside
the source tree.  churn and reload-sanity require a direct-library-compatible
elementary stream (WMV3 may use ASF).  --rc enforces the Phase 1 acceptance
duration/count for that mode; shorter configurable runs are development runs.
EOF
    exit 2
}

is_uint()
{
    value=$1
    maximum=$2
    case $value in
        ''|*[!0-9]*|0*) return 1 ;;
    esac
    [ "$value" -le "$maximum" ]
}

is_sha256()
{
    [ "${#1}" -eq 64 ] && printf '%s\n' "$1" | grep -Eq '^[0-9a-f]+$'
}

sha256_file()
{
    [ "$#" -eq 1 ] && [ -f "$1" ] && [ -r "$1" ] || return 1
    sha_line=$("$SHA256SUM" -- "$1") || return 1
    sha_value=${sha_line%% *}
    is_sha256 "$sha_value" || return 1
    printf '%s\n' "$sha_value"
}

freeze_release_environment()
{
    [ "$release_environment_overridden" = no ] ||
        die "PHASE1_* command, device, limit, or lock overrides are forbidden for release evidence"
    PATH=/usr/sbin:/usr/bin:/sbin:/bin
    export PATH
    FFPROBE=/usr/bin/ffprobe
    SHA256SUM=/usr/bin/sha256sum
    if [ -x /usr/bin/gnutimeout ]; then
        TIMEOUT=/usr/bin/gnutimeout
    else
        TIMEOUT=/usr/bin/timeout
    fi
    FUSER=/usr/bin/fuser
    MODULES_FILE=/proc/modules
    DEVICE=/dev/crystalhd
    SAMPLE_INTERVAL=1
    RSS_GROWTH_KB=32768
}

valid_rc_soak()
{
    geometry_value=$1
    seconds_value=$2
    profile_value=$3
    audio_value=$4
    [ "$profile_value" = High ] && [ "$audio_value" = yes ] || return 1
    case $geometry_value in
        1280x720) [ "$seconds_value" -eq 28800 ] ;;
        1920x1080) [ "$seconds_value" -ge 7200 ] && [ "$seconds_value" -le 14400 ] ;;
        *) return 1 ;;
    esac
}

valid_rc_controls()
{
    [ "$1" = 1280x720 ] && [ "$2" = High ] && [ "$3" = yes ] && \
        [ "$4" = no ] && [ "$5" -eq 10 ]
}

codec_name()
{
    case $1 in
        h264) printf '%s\n' h264 ;;
        mpeg2) printf '%s\n' mpeg2video ;;
        vc1) printf '%s\n' vc1 ;;
        wmv3) printf '%s\n' wmv3 ;;
        mpeg4) printf '%s\n' mpeg4 ;;
        *) return 1 ;;
    esac
}

epoch_field()
{
    case $1 in
        progressive) printf '%s\n' p ;;
        tt|bt|tff) printf '%s\n' tff ;;
        bb|tb|bff) printf '%s\n' bff ;;
        unknown) printf '%s\n' '' ;;
        *) return 1 ;;
    esac
}

probe_value()
{
    entry=$1
    file=$2
    "$FFPROBE" -v error -select_streams v:0 -show_entries "stream=$entry" \
        -of default=nw=1:nk=1 "$file" | sed -n '1p'
}

audio_probe_value()
{
    entry=$1
    file=$2
    "$FFPROBE" -v error -select_streams a:0 -show_entries "stream=$entry" \
        -of default=nw=1:nk=1 "$file" | sed -n '1p'
}

probe_frame_count()
{
    frame_file=$1
    declared_frames=$(probe_value nb_frames "$frame_file" 2>/dev/null || true)
    if is_uint "$declared_frames" 10000000; then
        printf '%s\n' "$declared_frames"
    else
        "$FFPROBE" -v error -select_streams v:0 -count_frames \
            -show_entries stream=nb_read_frames -of default=nw=1:nk=1 \
            "$frame_file"
    fi
}

validate_fixture()
{
    file=$1
    expected_sha=$2
    expected_codec=$3
    expected_profile=$4
    expected_geometry=$5
    expected_field=$6
    expected_frames=$7

    if [ ! -f "$file" ] || [ ! -r "$file" ]; then
        error "fixture is not a readable regular file: $file"
        return 1
    fi
    if ! is_sha256 "$expected_sha"; then
        error "fixture SHA-256 is not 64 lowercase hexadecimal characters"
        return 1
    fi
    actual_sha=$(sha256_file "$file") || return 1
    if [ "$actual_sha" != "$expected_sha" ]; then
        error "fixture SHA-256 mismatch: $file"
        return 1
    fi
    actual_codec=$(probe_value codec_name "$file") || return 1
    actual_profile=$(probe_value profile "$file") || return 1
    width=$(probe_value width "$file") || return 1
    height=$(probe_value height "$file") || return 1
    actual_field=$(probe_value field_order "$file") || return 1
    actual_frames=$(probe_frame_count "$file") || return 1
    [ -n "$actual_field" ] || actual_field=unknown

    if [ "$actual_codec" != "$expected_codec" ]; then
        error "fixture codec mismatch: expected $expected_codec, found ${actual_codec:-none}"
        return 1
    fi
    if [ "$actual_profile" != "$expected_profile" ]; then
        error "fixture profile mismatch: expected $expected_profile, found ${actual_profile:-none}"
        return 1
    fi
    if [ "${width}x${height}" != "$expected_geometry" ]; then
        error "fixture geometry mismatch: expected $expected_geometry, found ${width}x${height}"
        return 1
    fi
    if [ "$actual_field" != "$expected_field" ]; then
        error "fixture field order mismatch: expected $expected_field, found $actual_field"
        return 1
    fi
    if [ "$actual_frames" != "$expected_frames" ]; then
        error "fixture frame-count mismatch: expected $expected_frames, found ${actual_frames:-none}"
        return 1
    fi
}

validate_manifest()
{
    manifest_file=$1
    [ -f "$manifest_file" ] && [ -r "$manifest_file" ] || {
        error "oracle manifest is not a readable file: $manifest_file"
        return 1
    }
    [ -z "$(tail -c 1 -- "$manifest_file")" ] || {
        error "oracle manifest must end with a newline"
        return 1
    }
    awk -F '\t' '
        BEGIN { expected[1]="h264"; expected[2]="mpeg2"; expected[3]="vc1";
                expected[4]="wmv3"; expected[5]="mpeg4" }
        NR == 1 {
          if ($0 != "# crystalhd-oracle-v1 metadata=CHMD2 pixels=YUY2-active-row-v1") bad=1
          else header=1
          next
        }
        /^#/ || /^$/ { next }
        {
          row++
          split($7, geometry, "x")
          if ($1 == "round") {
            round++
            if ($3 != expected[round]) bad=1
          } else if ($1 != "conformance") bad=1
          if (NF != 11 || $2 !~ /^[a-z0-9][a-z0-9-]*$/ || seen[$2]++ ||
              $3 !~ /^(h264|mpeg2|vc1|wmv3|mpeg4)$/ ||
              $4 !~ /^[A-Za-z0-9][A-Za-z0-9._-]*$/ ||
              length($5) != 64 || $5 !~ /^[0-9a-f]+$/ ||
              $6 == "" || $6 == "-" || $7 !~ /^[1-9][0-9]*x[1-9][0-9]*$/ ||
              geometry[1] + 0 > 8192 || geometry[2] + 0 > 8192 ||
              $8 !~ /^(progressive|unknown|tt|tb|bb|bt)$/ ||
              $9 !~ /^[1-9][0-9]*$/ || $9 + 0 > 10000000 ||
              length($10) != 64 || $10 !~ /^[0-9a-f]+$/ ||
              length($11) != 64 || $11 !~ /^[0-9a-f]+$/ ||
              $5 ~ /^0+$/ || $10 ~ /^0+$/ || $11 ~ /^0+$/) bad=1
        }
        END { exit !(header && row >= 5 && round == 5 && !bad) }
    ' "$manifest_file" || {
        error "oracle must contain 11 valid TSV fields and exactly five round rows in h264, mpeg2, vc1, wmv3, mpeg4 order"
        return 1
    }
}

manifest_template()
{
    tab=$(printf '\t')
    printf '%s\n' '# crystalhd-oracle-v1 metadata=CHMD2 pixels=YUY2-active-row-v1'
    printf '# scope%sid%scodec%sfixture basename%sinput SHA-256%sprofile%sgeometry%sfield%sframes%smetadata SHA-256%sYUY2 SHA-256\n' \
        "$tab" "$tab" "$tab" "$tab" "$tab" "$tab" "$tab" "$tab" "$tab" "$tab"
    for item in h264 mpeg2 vc1 wmv3 mpeg4; do
        printf 'round%s%s-short%s%s%s%s.fixture%s<64 lowercase hex>%s<exact ffprobe profile>%s640x360%sprogressive%s1%s<64 lowercase hex>%s<64 lowercase hex>\n' \
            "$tab" "$item" "$tab" "$item" "$tab" "$item" "$tab" "$tab" \
            "$tab" "$tab" "$tab" "$tab" "$tab"
    done
}

module_refcount()
{
    awk '$1 == "crystalhd" { print $3; found=1 } END { if (!found) print "unloaded" }' \
        "$MODULES_FILE"
}

pin_net()
{
    awk '
        $1 == "nr_foll_pin_acquired" { acquired=$2; have_a=1 }
        $1 == "nr_foll_pin_released" { released=$2; have_r=1 }
        END { if (have_a && have_r) print acquired - released; else print "unavailable" }
    ' /proc/vmstat 2>/dev/null || printf '%s\n' unavailable
}

device_idle()
{
    ref=$(module_refcount) || return 1
    if [ "$ref" != 0 ]; then
        error "crystalhd module must be loaded with refcount 0 (found $ref)"
        return 1
    fi
    if "$FUSER" -s "$DEVICE" >/dev/null 2>&1; then
        error "$DEVICE is owned by another process"
        return 1
    else
        status=$?
        if [ "$status" -ne 1 ]; then
            error "could not determine whether $DEVICE is in use"
            return 1
        fi
    fi
    return 0
}

validate_bcm70015()
{
    count=0
    for pci_path in /sys/bus/pci/drivers/crystalhd/????:??:??.?; do
        [ -d "$pci_path" ] || continue
        count=$((count + 1))
        vendor=$(cat "$pci_path/vendor" 2>/dev/null || true)
        device=$(cat "$pci_path/device" 2>/dev/null || true)
        [ "$vendor" = 0x14e4 ] && [ "$device" = 0x1615 ] || {
            error "Phase 1 BCM70015 evidence cannot use PCI device ${vendor:-unknown}:${device:-unknown}"
            return 1
        }
    done
    [ "$count" -eq 1 ] || {
        error "Phase 1 BCM70015 evidence requires exactly one bound 14e4:1615 device (found $count)"
        return 1
    }
}

validate_bound_firmware()
{
    count=0
    for pci_path in /sys/bus/pci/drivers/crystalhd/????:??:??.?; do
        [ -d "$pci_path" ] || continue
        count=$((count + 1))
        device_id=$(cat "$pci_path/device" 2>/dev/null || true)
        case $device_id in
            0x1612) firmware=/lib/firmware/bcm70012fw.bin ;;
            0x1615) firmware=/lib/firmware/bcm70015fw.bin ;;
            *)
                error "unknown bound CrystalHD PCI device: ${device_id:-unavailable}"
                return 1
                ;;
        esac
        [ -f "$firmware" ] && [ -r "$firmware" ] || {
            error "firmware for bound device is unavailable: $firmware"
            return 1
        }
    done
    [ "$count" -gt 0 ] || {
        error "no bound CrystalHD PCI device was found"
        return 1
    }
}

set_runtime_env()
{
    export GST_REGISTRY=$registry
    if [ "$runtime" = source ]; then
        GST_PLUGIN_PATH=$plugin_dir
        GST_PLUGIN_PATH_1_0=$plugin_dir
        LD_LIBRARY_PATH=$repo_dir/linux_lib/libcrystalhd
        export GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0 LD_LIBRARY_PATH
        unset GST_PLUGIN_SYSTEM_PATH GST_PLUGIN_SYSTEM_PATH_1_0
    else
        unset GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
        unset GST_PLUGIN_SYSTEM_PATH GST_PLUGIN_SYSTEM_PATH_1_0 LD_LIBRARY_PATH
    fi
}

runtime_exec()
(
    set_runtime_env
    exec "$@"
)

capture_aer()
{
    output=$1
    : > "$output"
    for device_dir in /sys/bus/pci/drivers/crystalhd/????:??:??.?; do
        [ -d "$device_dir" ] || continue
        for counter in "$device_dir"/aer_dev_correctable \
            "$device_dir"/aer_dev_nonfatal "$device_dir"/aer_dev_fatal; do
            [ -r "$counter" ] || continue
            while IFS= read -r line; do
                printf '%s\t%s\n' "$counter" "$line" >> "$output"
            done < "$counter"
        done
    done
    [ -s "$output" ] || printf '%s\n' unavailable > "$output"
}

capture_snapshot()
{
    label=$1
    snapshot=$results/system-$label.txt
    {
        printf 'time=%s\n' "$(date --iso-8601=ns)"
        printf 'module_refcount=%s\n' "$(module_refcount)"
        printf 'pin_net=%s\n' "$(pin_net)"
        printf 'loaded_srcversion=%s\n' "$(cat /sys/module/crystalhd/srcversion 2>/dev/null || printf unavailable)"
        printf 'force_l0s_off=%s\n' "$(cat /sys/module/crystalhd/parameters/force_l0s_off 2>/dev/null || printf unavailable)"
        printf 'device=%s\n' "$DEVICE"
        ls -l -- "$DEVICE" 2>&1 || true
        "$FUSER" -v "$DEVICE" 2>&1 || true
    } > "$snapshot"
    capture_aer "$results/aer-$label.txt"
}

loaded_module_path()
{
    modinfo -n crystalhd 2>/dev/null
}

resolved_command()
{
    command_file=$(command -v "$1") || return 1
    case $command_file in /*) ;; *) return 1 ;; esac
    readlink -f -- "$command_file"
}

extract_module_build_note()
{
    module_file=$1
    note_file=$2
    module_image=$results/selected-module-image.tmp
    case $module_file in
        *.zst) zstdcat -- "$module_file" > "$module_image" || return 1 ;;
        *.xz) xzcat -- "$module_file" > "$module_image" || return 1 ;;
        *.gz) gzip -cd -- "$module_file" > "$module_image" || return 1 ;;
        *) cp -- "$module_file" "$module_image" || return 1 ;;
    esac
    if ! objcopy --dump-section .note.gnu.build-id="$note_file.tmp" \
        "$module_image" >/dev/null 2>&1; then
        rm -f -- "$module_image" "$note_file.tmp"
        return 1
    fi
    rm -f -- "$module_image"
    [ -s "$note_file.tmp" ] || {
        rm -f -- "$note_file.tmp"
        return 1
    }
    mv -f -- "$note_file.tmp" "$note_file"
}

write_runtime_fingerprint()
{
    output=$1
    inspect=$results/gst-inspect-crystalhd.txt
    runtime_exec gst-inspect-1.0 crystalhddec > "$inspect.tmp" 2>&1 || return 1
    plugin=$(awk '/^[[:space:]]*Filename[[:space:]]/ { print $2; exit }' "$inspect.tmp")
    [ -n "$plugin" ] && [ -f "$plugin" ] || return 1
    mv -f -- "$inspect.tmp" "$inspect"

    ldd_output=$results/library-drain.ldd
    runtime_exec ldd "$script_dir/library-drain-test" > "$ldd_output.tmp" 2>&1 || return 1
    library=$(awk '$1 ~ /^libcrystalhd\.so/ { print $3; exit }' "$ldd_output.tmp")
    [ -n "$library" ] && [ -f "$library" ] || return 1
    mv -f -- "$ldd_output.tmp" "$ldd_output"
    plugin=$(readlink -f -- "$plugin")
    library=$(readlink -f -- "$library")
    source_plugin=$(readlink -f -- "$plugin_dir/libgstcrystalhd.so")
    source_library=$(readlink -f -- "$repo_dir/linux_lib/libcrystalhd/libcrystalhd.so")
    plugin_hash=$(sha256_file "$plugin") || return 1
    library_hash=$(sha256_file "$library") || return 1
    source_plugin_hash=$(sha256_file "$source_plugin") || return 1
    source_library_hash=$(sha256_file "$source_library") || return 1
    if [ "$runtime" = source ]; then
        [ "$plugin" = "$source_plugin" ] || return 1
        [ "$library" = "$source_library" ] || return 1
    else
        [ "$plugin" != "$source_plugin" ] || return 1
        [ "$library" != "$source_library" ] || return 1
        [ "$plugin_hash" = "$source_plugin_hash" ] || return 1
        [ "$library_hash" = "$source_library_hash" ] || return 1
    fi
    validate_bound_firmware || return 1

    os_hash=$(sha256_file /etc/os-release) || return 1
    selected=$(loaded_module_path) || return 1
    selected=$(readlink -f -- "$selected") || return 1
    [ -f "$selected" ] && [ -r "$selected" ] || return 1
    selected_hash=$(sha256_file "$selected") || return 1
    selected_srcversion=$(modinfo -F srcversion "$selected" 2>/dev/null) || return 1
    selected_vermagic=$(modinfo -F vermagic "$selected" 2>/dev/null) || return 1
    loaded_srcversion=$(cat /sys/module/crystalhd/srcversion 2>/dev/null) || return 1
    [ -n "$selected_srcversion" ] && [ "$loaded_srcversion" = "$selected_srcversion" ] || return 1
    [ -n "$selected_vermagic" ] || return 1
    selected_note=$results/selected-module-build-note.bin
    extract_module_build_note "$selected" "$selected_note" || return 1
    selected_note_hash=$(sha256_file "$selected_note") || return 1
    loaded_note=/sys/module/crystalhd/notes/.note.gnu.build-id
    loaded_note_hash=$(sha256_file "$loaded_note") || return 1
    [ "$selected_note_hash" = "$loaded_note_hash" ] || return 1
    firmware_15_hash=$(sha256_file /lib/firmware/bcm70015fw.bin) || return 1
    firmware_12_hash=
    if [ -f /lib/firmware/bcm70012fw.bin ]; then
        firmware_12_hash=$(sha256_file /lib/firmware/bcm70012fw.bin) || return 1
    fi
    drain_hash=$(sha256_file "$script_dir/library-drain-test") || return 1
    playback_hash=$(sha256_file "$plugin_dir/gstreamer-playback-test") || return 1
    codec_hash=$(sha256_file "$plugin_dir/gstreamer-codec-playback-test") || return 1
    controls_hash=$(sha256_file "$plugin_dir/gstreamer-controls-test") || return 1
    ffprobe_path=$(resolved_command "$FFPROBE") || return 1
    sha256sum_path=$(resolved_command "$SHA256SUM") || return 1
    timeout_path=$(resolved_command "$TIMEOUT") || return 1
    fuser_path=$(resolved_command "$FUSER") || return 1
    objcopy_path=$(resolved_command objcopy) || return 1
    zstdcat_path=$(resolved_command zstdcat) || return 1
    ffprobe_hash=$(sha256_file "$ffprobe_path") || return 1
    sha256sum_hash=$(sha256_file "$sha256sum_path") || return 1
    timeout_hash=$(sha256_file "$timeout_path") || return 1
    fuser_hash=$(sha256_file "$fuser_path") || return 1
    objcopy_hash=$(sha256_file "$objcopy_path") || return 1
    zstdcat_hash=$(sha256_file "$zstdcat_path") || return 1

    {
        printf 'git_revision\t%s\n' "$(git -C "$repo_dir" rev-parse HEAD)"
        printf 'runtime\t%s\n' "$runtime"
        printf 'os_release\t%s\n' "$os_hash"
        printf 'plugin\t%s\t%s\n' "$plugin" "$plugin_hash"
        printf 'library\t%s\t%s\n' "$library" "$library_hash"
        printf 'loaded_module_srcversion\t%s\n' "$loaded_srcversion"
        printf 'selected_module\t%s\n' "$selected"
        printf 'selected_module_sha256\t%s\n' "$selected_hash"
        printf 'selected_module_srcversion\t%s\n' "$selected_srcversion"
        printf 'selected_module_vermagic\t%s\n' "$selected_vermagic"
        printf 'selected_module_build_note_sha256\t%s\n' "$selected_note_hash"
        printf 'loaded_module_build_note_sha256\t%s\n' "$loaded_note_hash"
        [ -z "$firmware_12_hash" ] || \
            printf 'firmware\t%s\t%s\n' /lib/firmware/bcm70012fw.bin "$firmware_12_hash"
        printf 'firmware\t%s\t%s\n' /lib/firmware/bcm70015fw.bin "$firmware_15_hash"
        printf 'probe\t%s\t%s\n' "$script_dir/library-drain-test" "$drain_hash"
        printf 'probe\t%s\t%s\n' "$plugin_dir/gstreamer-playback-test" "$playback_hash"
        printf 'probe\t%s\t%s\n' "$plugin_dir/gstreamer-codec-playback-test" "$codec_hash"
        printf 'probe\t%s\t%s\n' "$plugin_dir/gstreamer-controls-test" "$controls_hash"
        printf 'tool\t%s\t%s\n' "$ffprobe_path" "$ffprobe_hash"
        printf 'tool\t%s\t%s\n' "$sha256sum_path" "$sha256sum_hash"
        printf 'tool\t%s\t%s\n' "$timeout_path" "$timeout_hash"
        printf 'tool\t%s\t%s\n' "$fuser_path" "$fuser_hash"
        printf 'tool\t%s\t%s\n' "$objcopy_path" "$objcopy_hash"
        printf 'tool\t%s\t%s\n' "$zstdcat_path" "$zstdcat_hash"
    } > "$output"
}

write_identity()
{
    identity=$results/identity.txt
    identity_os_hash=$(sha256_file /etc/os-release) || return 1
    identity_script_hash=$(sha256_file "$script_dir/phase1-release-gate.sh") || return 1
    source_module=$repo_dir/driver/linux/crystalhd.ko
    [ -f "$source_module" ] && [ -r "$source_module" ] || return 1
    identity_module_srcversion=$(modinfo -F srcversion "$source_module" 2>/dev/null) || return 1
    identity_module_hash=$(sha256_file "$source_module") || return 1
    [ -n "$identity_module_srcversion" ] || return 1
    {
        printf 'started=%s\n' "$started_at"
        printf 'mode=%s\n' "$mode"
        printf 'arguments=%s\n' "$original_args"
        printf 'source=%s\n' "$repo_dir"
        printf 'revision=%s\n' "$revision"
        printf 'runtime=%s\n' "$runtime"
        printf 'rc=%s\n' "$rc"
        printf 'acceptance=%s\n' "$acceptance"
        printf 'kernel=%s\n' "$(uname -srvmo)"
        printf 'os_release_sha256=%s\n' "$identity_os_hash"
        sed -n 's/^ID=/os_id=/p; s/^VERSION_ID=/os_version_id=/p; s/^VERSION_CODENAME=/os_version_codename=/p; s/^PRETTY_NAME=/os_pretty_name=/p' \
            /etc/os-release
        printf 'timeout=%s\n' "$("$TIMEOUT" --version | sed -n '1p')"
        printf 'ffprobe=%s\n' "$("$FFPROBE" -version | sed -n '1p')"
        printf 'gstreamer=%s\n' "$(gst-inspect-1.0 --version | sed -n '1p')"
        printf 'script_sha256=%s\n' "$identity_script_hash"
        printf 'source_module_srcversion=%s\n' "$identity_module_srcversion"
        printf 'source_module_sha256=%s\n' "$identity_module_hash"
        for pci_path in /sys/bus/pci/drivers/crystalhd/????:??:??.?; do
            [ -d "$pci_path" ] || continue
            pci_bdf=$(basename "$(readlink -f -- "$pci_path")")
            printf 'pci_bdf=%s\n' "$pci_bdf"
            for attribute in vendor device subsystem_vendor subsystem_device revision class; do
                [ -r "$pci_path/$attribute" ] || continue
                printf 'pci_%s=%s\n' "$attribute" "$(cat "$pci_path/$attribute")"
            done
        done
    } > "$identity"
}

read_process_metrics()
{
    [ "$#" -eq 1 ] || return 1
    awk '
        $1 == "State:" { state=$2; have_state=1 }
        $1 == "VmRSS:" { rss=$2; have_rss=1 }
        $1 == "Threads:" { threads=$2; have_threads=1 }
        END {
          if (!have_state) exit 1
          if (state == "Z" || state == "X" || state == "x") {
            print "exited"
            exit
          }
          if (!have_rss || !have_threads) exit 1
          printf "live %s %s\n", rss, threads
        }
    ' "$1" 2>/dev/null
}

process_is_live()
{
    process_dir=$1
    [ -d "$process_dir" ] || return 1
    process_state=$(awk '
        $1 == "State:" { print $2; found=1; exit }
        END { if (!found) exit 1 }
    ' "$process_dir/status" 2>/dev/null) || {
        [ -d "$process_dir" ] || return 1
        return 2
    }
    case $process_state in Z|X|x) return 1 ;; *) return 0 ;; esac
}

watchdog_is_live()
{
    watchdog_pid=$1
    kill -0 "$watchdog_pid" 2>/dev/null || return 1
    if process_is_live "$PROC_ROOT/$watchdog_pid"; then
        return 0
    else
        watchdog_state=$?
    fi
    [ "$watchdog_state" -ne 1 ]
}

count_process_fds()
{
    fd_dir=$1
    [ -r "$fd_dir" ] && [ -x "$fd_dir" ] || return 1
    set -- "$fd_dir"/*
    if [ "$#" -eq 1 ] && [ "$1" = "$fd_dir/*" ]; then
        fd_count=0
    else
        fd_count=$#
    fi
    [ -d "$fd_dir" ] || return 1
    printf '%s\n' "$fd_count"
}

sample_process_list()
{
    stage=$1
    sample=$2
    shift 2
    rss=0
    fds=0
    threads=0
    count=0
    for pid do
        process_dir=$PROC_ROOT/$pid
        status_file=$process_dir/status
        if metrics=$(read_process_metrics "$status_file"); then
            [ "$metrics" != exited ] || continue
        else
            if process_is_live "$process_dir"; then
                return 1
            else
                process_state=$?
            fi
            [ "$process_state" -eq 1 ] && continue
            return 1
        fi
        case $metrics in live\ *) ;; *) return 1 ;; esac
        metrics=${metrics#live }
        one_rss=${metrics%% *}
        one_threads=${metrics#* }
        case $one_rss in ''|*[!0-9]*) return 1 ;; esac
        case $one_threads in ''|*[!0-9]*|0) return 1 ;; esac
        if one_fds=$(count_process_fds "$process_dir/fd"); then
            :
        else
            if process_is_live "$process_dir"; then
                return 1
            else
                process_state=$?
            fi
            [ "$process_state" -eq 1 ] && continue
            return 1
        fi
        if process_is_live "$process_dir"; then
            :
        else
            process_state=$?
            [ "$process_state" -eq 1 ] && continue
            return 1
        fi
        rss=$((rss + one_rss))
        threads=$((threads + one_threads))
        fds=$((fds + one_fds))
        count=$((count + 1))
    done
    [ "$count" -gt 0 ] || return 1
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$stage" "$sample" \
        "$(date +%s)" "$rss" "$fds" "$threads" "$count" >> "$results/resources.tsv"
}

sample_process_tree()
{
    watchdog=$1
    stage=$2
    sample=$3
    children=$(ps -o pid= --ppid "$watchdog" 2>/dev/null || true)
    set -- "$watchdog"
    for child in $children; do
        set -- "$@" "$child"
    done
    sample_process_list "$stage" "$sample" "$@"
}

monitor_process()
{
    watchdog=$1
    stage=$2
    sample=1
    while watchdog_is_live "$watchdog"; do
        if [ "$sample" -lt "$RESOURCE_STARTUP_SAMPLES" ]; then
            sleep 0.1
        else
            sleep "$SAMPLE_INTERVAL"
        fi
        watchdog_is_live "$watchdog" || break
        sample=$((sample + 1))
        if ! sample_process_tree "$watchdog" "$stage" "$sample"; then
            # A live watchdog without an auditable process-tree sample makes
            # the resource record incomplete.  Stop the stage so its success
            # cannot hide a failed sampler.
            if watchdog_is_live "$watchdog"; then
                kill -TERM "$watchdog" 2>/dev/null || true
                return 1
            fi
            break
        fi
    done
}

check_resource_growth()
{
    stage=$1
    series=${2:-no}
    minimum_post_startup=${3:-0}
    awk -F '\t' -v stage="$stage" -v series="$series" \
        -v minimum_post_startup="$minimum_post_startup" \
        -v startup_samples="$RESOURCE_STARTUP_SAMPLES" \
        -v trend_samples="$RESOURCE_TREND_SAMPLES" \
        -v rss_limit="$RSS_GROWTH_KB" '
        $1 == stage || (series == "yes" && $1 ~ ("-" stage "$") ) {
          n++
          rss[n]=$4+0; fd[n]=$5+0; thread[n]=$6+0
          if (n == 1 || rss[n] > max_rss) max_rss=rss[n]
        }
        END {
          if (n == 0) exit 2
          post_startup=n-startup_samples
          if (post_startup < 0) post_startup=0
          trend_checked=(post_startup >= trend_samples)
          if (post_startup > 0) {
            first=startup_samples+1
            window=int(post_startup/10); if (window < 3) window=3
            if (!trend_checked) window=post_startup
          } else {
            first=1
            window=n
          }
          last=trend_checked ? n-window+1 : first
          first_rss=first_fd=first_thread=last_rss=last_fd=last_thread=0
          for (i=first; i<first+window; i++) {
            first_rss+=rss[i]; first_fd+=fd[i]; first_thread+=thread[i]
          }
          for (i=last; i<last+window; i++) {
            last_rss+=rss[i]; last_fd+=fd[i]; last_thread+=thread[i]
          }
          first_rss/=window; first_fd/=window; first_thread/=window
          last_rss/=window; last_fd/=window; last_thread/=window
          printf "%s\tsamples=%d\tRSS-early-avg=%.0f\tRSS-late-avg=%.0f\tRSS-max=%d\tFD-early-avg=%.1f\tFD-late-avg=%.1f\tthreads-early-avg=%.1f\tthreads-late-avg=%.1f\ttrend=%s\n",
              stage, n, first_rss, last_rss, max_rss, first_fd, last_fd,
              first_thread, last_thread, (trend_checked ? "checked" : "insufficient-short-stage")
          if (post_startup < minimum_post_startup) exit 3
          if (trend_checked && last_rss > first_rss + rss_limit) exit 1
          if (trend_checked && last_fd > first_fd + 8) exit 1
          if (trend_checked && last_thread > first_thread + 4) exit 1
        }
    ' "$results/resources.tsv" >> "$results/resource-summary.tsv" || {
        error "process resource sampling failed or late-window growth exceeded the release bound in $stage"
        return 1
    }
}

run_timed()
{
    device_idle || return 1
    stage_number=$((stage_number + 1))
    stage=$(printf '%04d-%s' "$stage_number" "$1")
    shift
    wall=$1
    shift
    log=$results/stages/$stage.log
    progress=$results/stages/$stage.progress
    printf '%s\t%s\t' "$stage" "$wall" >> "$results/stages.tsv"
    (
        set_runtime_env
        PHASE1_PROGRESS_FILE=$progress
        export PHASE1_PROGRESS_FILE
        exec "$TIMEOUT" --signal=TERM --kill-after=30 "$wall" "$@"
    ) > "$log" 2>&1 &
    active_watchdog=$!
    sample_process_tree "$active_watchdog" "$stage" 1 || {
        error "could not take the initial process-resource sample for $stage"
        terminate_active
        return 1
    }
    monitor_process "$active_watchdog" "$stage" &
    active_monitor=$!
    if wait "$active_watchdog"; then
        status=0
    else
        status=$?
    fi
    active_watchdog=
    if wait "$active_monitor" 2>/dev/null; then
        monitor_status=0
    else
        monitor_status=$?
    fi
    active_monitor=
    printf '%s\n' "$status" >> "$results/stages.tsv"
    if [ "$monitor_status" -ne 0 ]; then
        error "process-resource monitor failed for $stage"
        return 1
    fi
    if [ "$status" -ne 0 ]; then
        error "stage $stage failed with status $status; full log: $log"
        if [ -s "$progress" ]; then
            printf '%s\n' 'last complete probe progress:' >&2
            tail -n 4 "$progress" >&2
        fi
        sed -n '1,240p' "$log" >&2
        return 1
    fi
    [ -s "$progress" ] || {
        error "stage $stage completed without a probe progress record"
        return 1
    }
    minimum_post_startup=0
    if [ "$rc" = yes ]; then
        case $mode in
            soak|churn) minimum_post_startup=$RESOURCE_TREND_SAMPLES ;;
        esac
    fi
    check_resource_growth "$stage" no "$minimum_post_startup" || return 1
    sed -n '1,240p' "$log"
}

extract_metadata_sha()
{
    sed -n 's/.*MetadataSHA256=\([0-9a-f]\{64\}\); SHA256=.*/\1/p' "$1" | tail -n 1
}

extract_pixel_sha()
{
    sed -n 's/.*; SHA256=\([0-9a-f]\{64\}\)$/\1/p' "$1" | tail -n 1
}

capture_kernel_log()
{
    [ -n "$started_at" ] || return 0
    journalctl --quiet -k --since "$started_at" --no-pager > "$results/kernel.log" 2>&1 || \
        printf '%s\n' 'kernel journal capture failed' >> "$results/kernel.log"
}

capture_final_artifacts()
{
    [ "$initialized" = yes ] || return 0
    capture_snapshot after 2>> "$results/artifact-errors.log" || true
    write_runtime_fingerprint "$results/runtime-fingerprint.after.tsv" \
        2>> "$results/artifact-errors.log" || true
    capture_kernel_log || true
}

root_exec()
{
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo -n "$@"
    fi
}

normalized_force_l0s()
{
    case $1 in
        Y|y|1) printf '%s\n' 1 ;;
        N|n|0) printf '%s\n' 0 ;;
        *) return 1 ;;
    esac
}

attest_release_reload()
{
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || die "release evidence needs root or non-interactive sudo"
        sudo -n true >/dev/null 2>&1 || die "release evidence needs root or non-interactive sudo"
    fi
    device_idle || return 1
    reload_selected=$(loaded_module_path) || return 1
    reload_selected=$(readlink -f -- "$reload_selected") || return 1
    [ -f "$reload_selected" ] && [ -r "$reload_selected" ] || return 1
    reload_selected_hash=$(sha256_file "$reload_selected") || return 1
    reload_selected_srcversion=$(modinfo -F srcversion "$reload_selected" 2>/dev/null) || return 1
    reload_selected_vermagic=$(modinfo -F vermagic "$reload_selected" 2>/dev/null) || return 1
    reload_note=$results/reload-selected-build-note.bin
    extract_module_build_note "$reload_selected" "$reload_note" || return 1
    reload_note_hash=$(sha256_file "$reload_note") || return 1
    reload_firmware_hash=$(sha256_file /lib/firmware/bcm70015fw.bin) || return 1
    force_before=$(cat /sys/module/crystalhd/parameters/force_l0s_off 2>/dev/null) || return 1
    reload_force_value=$(normalized_force_l0s "$force_before") || return 1

    reload_in_progress=yes
    {
        root_exec "$TIMEOUT" --foreground --kill-after=5 30 modprobe -r crystalhd
        [ ! -d /sys/module/crystalhd ]
        root_exec "$TIMEOUT" --foreground --kill-after=5 30 \
            modprobe crystalhd force_l0s_off="$reload_force_value"
        udevadm settle --timeout=10
    } > "$results/module-reload.log" 2>&1 || {
        sed -n '1,240p' "$results/module-reload.log" >&2
        die "controlled release module reload failed"
    }
    reload_in_progress=no
    [ -r "$DEVICE" ] && [ -w "$DEVICE" ] || return 1
    device_idle || return 1
    validate_bcm70015 || return 1

    reload_selected_after=$(loaded_module_path) || return 1
    reload_selected_after=$(readlink -f -- "$reload_selected_after") || return 1
    reload_selected_hash_after=$(sha256_file "$reload_selected_after") || return 1
    reload_firmware_hash_after=$(sha256_file /lib/firmware/bcm70015fw.bin) || return 1
    loaded_srcversion_after=$(cat /sys/module/crystalhd/srcversion 2>/dev/null) || return 1
    loaded_note_hash_after=$(sha256_file \
        /sys/module/crystalhd/notes/.note.gnu.build-id) || return 1
    force_after=$(cat /sys/module/crystalhd/parameters/force_l0s_off 2>/dev/null) || return 1
    [ "$reload_selected_after" = "$reload_selected" ] && \
        [ "$reload_selected_hash_after" = "$reload_selected_hash" ] && \
        [ "$reload_firmware_hash_after" = "$reload_firmware_hash" ] && \
        [ "$loaded_srcversion_after" = "$reload_selected_srcversion" ] && \
        [ "$loaded_note_hash_after" = "$reload_note_hash" ] && \
        [ "$(normalized_force_l0s "$force_after")" = "$reload_force_value" ] || return 1

    {
        printf 'result\tPASS\n'
        printf 'selected_module\t%s\n' "$reload_selected"
        printf 'selected_module_sha256\t%s\n' "$reload_selected_hash"
        printf 'selected_module_srcversion\t%s\n' "$reload_selected_srcversion"
        printf 'selected_module_vermagic\t%s\n' "$reload_selected_vermagic"
        printf 'selected_module_build_note_sha256\t%s\n' "$reload_note_hash"
        printf 'loaded_module_build_note_sha256\t%s\n' "$loaded_note_hash_after"
        printf 'firmware\t%s\t%s\n' /lib/firmware/bcm70015fw.bin "$reload_firmware_hash"
        printf 'force_l0s_off_before\t%s\n' "$force_before"
        printf 'force_l0s_off_after\t%s\n' "$force_after"
        printf 'pci\t14e4:1615\n'
    } > "$results/module-reload.tsv"
}

recover_reload()
{
    [ "$reload_in_progress" = yes ] || return 0
    printf '%s\n' 'attempting crystalhd module recovery after an interrupted reload' \
        >> "$results/reload-recovery.log"
    root_exec "$TIMEOUT" --foreground --kill-after=5 30 \
        modprobe crystalhd force_l0s_off="$reload_force_value" \
        >> "$results/reload-recovery.log" 2>&1 || true
    reload_in_progress=no
}

terminate_active()
{
    if [ -n "$active_watchdog" ] && kill -0 "$active_watchdog" 2>/dev/null; then
        children=$(ps -o pid= --ppid "$active_watchdog" 2>/dev/null || true)
        for child in $children; do
            kill -TERM "$child" 2>/dev/null || true
        done
        kill -TERM "$active_watchdog" 2>/dev/null || true
        attempts=0
        while kill -0 "$active_watchdog" 2>/dev/null && [ "$attempts" -lt 50 ]; do
            sleep 0.1
            attempts=$((attempts + 1))
        done
        if kill -0 "$active_watchdog" 2>/dev/null; then
            for child in $children; do
                kill -KILL "$child" 2>/dev/null || true
            done
            kill -KILL "$active_watchdog" 2>/dev/null || true
        fi
        wait "$active_watchdog" 2>/dev/null || true
    fi
    active_watchdog=
    if [ -n "$active_monitor" ]; then
        kill -TERM "$active_monitor" 2>/dev/null || true
        wait "$active_monitor" 2>/dev/null || true
    fi
    active_monitor=
}

on_signal()
{
    trap - HUP INT TERM
    set +e
    terminate_active
    exit 130
}

on_exit()
{
    status=$?
    trap - EXIT HUP INT TERM
    set +e
    terminate_active
    recover_reload
    if [ "$initialized" = yes ] && [ "$finished" != yes ]; then
        capture_final_artifacts
        printf 'result=FAIL\nstatus=%s\nrevision=%s\nruntime=%s\nmode=%s\nrc=%s\nacceptance=%s\n' \
            "$status" "${revision:-unavailable}" "${runtime:-unavailable}" \
            "${mode:-unavailable}" "$rc" "$acceptance" > "$results/result.txt"
        [ "$status" -ne 0 ] || status=1
    fi
    exit "$status"
}

require_commands()
{
    for command in git realpath flock "$FUSER" "$TIMEOUT" "$FFPROBE" "$SHA256SUM" \
        awk sed grep find ps journalctl modinfo modprobe udevadm gst-inspect-1.0 ldd \
        make readlink cp cmp diff stat dirname basename tail wc date sleep objcopy zstdcat; do
        command -v "$command" >/dev/null 2>&1 || die "required command is missing: $command"
    done
    "$TIMEOUT" --version 2>/dev/null | grep -q 'GNU coreutils' || \
        die "GNU coreutils timeout is required"
    watchdog_works "$TIMEOUT" || \
        die "timeout does not enforce the required TERM/KILL watchdog"
    is_uint "$SAMPLE_INTERVAL" 300 || die "PHASE1_SAMPLE_INTERVAL must be 1..300"
    is_uint "$RSS_GROWTH_KB" 1048576 || die "PHASE1_RSS_GROWTH_KB must be 1..1048576"
}

clean_tree()
{
    git -C "$repo_dir" diff --quiet --ignore-submodules -- &&
        git -C "$repo_dir" diff --cached --quiet --ignore-submodules -- &&
        [ -z "$(git -C "$repo_dir" status --porcelain --untracked-files=normal)" ]
}

prepare_probes()
{
    {
        make -B -C "$repo_dir" driver library-drain-test
        make -B -C "$plugin_dir" all gstreamer-playback-test \
            gstreamer-controls-test codec-playback-test
    } > "$results/build.log" 2>&1 || {
        sed -n '1,240p' "$results/build.log" >&2
        die "probe build failed"
    }
    for probe in "$script_dir/library-drain-test" "$plugin_dir/gstreamer-playback-test" \
        "$plugin_dir/gstreamer-controls-test" "$plugin_dir/gstreamer-codec-playback-test"; do
        [ -x "$probe" ] || die "required probe was not built: $probe"
    done
}

prepare_run()
{
    require_commands
    case $runtime in source|installed) ;; *) die "--runtime must be source or installed" ;; esac
    [ -n "$results" ] || die "--results is required"
    case $results in /*) ;; *) die "--results must be an absolute path" ;; esac
    results=$(realpath -m -- "$results")
    case "$results/" in "$repo_dir/"*) die "results directory must be outside the source tree" ;; esac
    [ ! -e "$results" ] || die "results path already exists: $results"

    default_lock_dir=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
    lock_file=${PHASE1_LOCK_FILE:-$default_lock_dir/crystalhd-phase1-release.lock}
    lock_parent=$(dirname -- "$lock_file")
    [ -d "$lock_parent" ] || die "release-gate lock directory is unavailable: $lock_parent"
    lock_owner=$(stat -c %u -- "$lock_parent") || die "cannot inspect lock directory"
    lock_mode=$(stat -c %a -- "$lock_parent") || die "cannot inspect lock directory permissions"
    [ "$lock_owner" -eq "$(id -u)" ] || die "release-gate lock directory is not owned by this user"
    [ $((0$lock_mode & 0022)) -eq 0 ] || die "release-gate lock directory is group/world writable"
    [ ! -L "$lock_file" ] || die "release-gate lock must not be a symbolic link"
    exec 9>> "$lock_file" || die "cannot open release-gate lock: $lock_file"
    flock -n 9 || die "another CrystalHD release gate holds $lock_file"

    mkdir -m 0750 -- "$results" || die "cannot atomically create results directory"
    [ ! -L "$results" ] && [ -d "$results" ] && \
        [ "$(stat -c %u -- "$results")" -eq "$(id -u)" ] && \
        [ $((0$(stat -c %a -- "$results") & 0027)) -eq 0 ] || \
        die "results directory ownership or permissions are unsafe"
    mkdir -m 0750 -- "$results/stages" || die "cannot create results stage directory"
    registry=$results/gstreamer-registry.bin
    started_at=$(date '+%Y-%m-%d %H:%M:%S.%6N')
    initialized=yes
    trap on_exit EXIT
    trap on_signal HUP INT TERM

    clean_tree || die "release gates require a clean source tree, including no untracked files"
    revision=$(git -C "$repo_dir" rev-parse HEAD) || die "cannot identify source revision"
    [ -r "$DEVICE" ] && [ -w "$DEVICE" ] || die "$DEVICE is missing or inaccessible"
    device_idle || exit 1
    if [ "$rc" = yes ] || [ "$mode" = oracle ]; then
        validate_bcm70015 || exit 1
    fi

    printf 'stage\twall-timeout-seconds\texit-status\n' > "$results/stages.tsv"
    printf 'stage\tsample\tepoch\trss-kb\tfds\tthreads\tprocesses\n' > "$results/resources.tsv"
    printf 'stage\tresource-summary\n' > "$results/resource-summary.tsv"
    prepare_probes
    clean_tree || die "building probes changed tracked source files"
    if [ "$rc" = yes ] || [ "$mode" = oracle ]; then
        attest_release_reload || die "could not attest a controlled release module/firmware reload"
    fi
    write_identity || die "could not record release identity"
    write_runtime_fingerprint "$results/runtime-fingerprint.before.tsv" || \
        die "could not identify the selected $runtime library and GStreamer plugin"

    loaded=$(cat /sys/module/crystalhd/srcversion 2>/dev/null || true)
    selected=$(loaded_module_path || true)
    selected_version=$(modinfo -F srcversion "$selected" 2>/dev/null || true)
    [ -n "$loaded" ] && [ "$loaded" = "$selected_version" ] || \
        die "loaded and selected crystalhd modules do not match"
    if [ -f "$repo_dir/driver/linux/crystalhd.ko" ]; then
        source_version=$(modinfo -F srcversion "$repo_dir/driver/linux/crystalhd.ko" 2>/dev/null || true)
        [ -n "$source_version" ] && [ "$loaded" = "$source_version" ] || \
            die "loaded and source crystalhd modules do not match"
    fi
    capture_snapshot before
}

finish_run()
{
    verify_fixtures_unchanged || die "one or more release fixtures changed during the gate"
    capture_snapshot after
    write_runtime_fingerprint "$results/runtime-fingerprint.after.tsv" || \
        die "could not re-identify runtime after the gate"
    before_ref=$(sed -n 's/^module_refcount=//p' "$results/system-before.txt")
    after_ref=$(sed -n 's/^module_refcount=//p' "$results/system-after.txt")
    [ "$before_ref" = 0 ] && [ "$after_ref" = 0 ] || die "module refcount did not return to zero"
    before_pin=$(sed -n 's/^pin_net=//p' "$results/system-before.txt")
    after_pin=$(sed -n 's/^pin_net=//p' "$results/system-after.txt")
    printf '%s\n' "$before_pin" | grep -Eq '^[0-9]+$' && \
        [ "$before_pin" = "$after_pin" ] || \
        die "global long-term pin balance is unavailable or changed: $before_pin -> $after_pin"
    before_force=$(sed -n 's/^force_l0s_off=//p' "$results/system-before.txt")
    after_force=$(sed -n 's/^force_l0s_off=//p' "$results/system-after.txt")
    printf '%s\n' "$before_force" | grep -Eq '^[YN]$' && \
        [ "$before_force" = "$after_force" ] || \
        die "force_l0s_off is unavailable or changed: $before_force -> $after_force"
    diff -u "$results/aer-before.txt" "$results/aer-after.txt" > "$results/aer.diff" || \
        die "CrystalHD AER counters changed during the gate"
    diff -u "$results/runtime-fingerprint.before.tsv" \
        "$results/runtime-fingerprint.after.tsv" > "$results/runtime-fingerprint.diff" || \
        die "source, runtime, module, firmware, or probe identity changed during the gate"
    [ "$(git -C "$repo_dir" rev-parse HEAD)" = "$revision" ] && clean_tree || \
        die "source revision or worktree changed during the gate"
    device_idle || exit 1
    if sh "$script_dir/kernel-log-check.sh" "$started_at" > "$results/kernel-check.log" 2>&1; then
        :
    else
        sed -n '1,240p' "$results/kernel-check.log" >&2
        die "kernel log validation failed"
    fi
    capture_kernel_log
    printf 'result=PASS\nrevision=%s\nruntime=%s\nmode=%s\nrc=%s\nacceptance=%s\n' \
        "$revision" "$runtime" "$mode" "$rc" "$acceptance" > "$results/result.txt"
    finished=yes
    if [ "$rc" = yes ]; then
        say "Phase 1 RC $mode gate passed; evidence: $results"
    else
        say "Phase 1 development $mode run passed; evidence: $results"
    fi
}

verify_fixtures_unchanged()
{
    if [ "$mode" = oracle ] || [ "$mode" = round-robin ]; then
        tab=$(printf '\t')
        while IFS="$tab" read -r scope row_id key file expected_sha rest; do
            actual_sha=$(sha256_file "$file" 2>/dev/null) || return 1
            [ "$actual_sha" = "$expected_sha" ] || return 1
        done < "$results/resolved-oracle.tsv"
    else
        file=$(sed -n 's/^fixture=//p' "$results/fixture.txt")
        expected_sha=$(sed -n 's/^sha256=//p' "$results/fixture.txt")
        actual_sha=$(sha256_file "$file" 2>/dev/null) || return 1
        [ "$actual_sha" = "$expected_sha" ] || return 1
    fi
}

validate_common_fixture()
{
    [ -n "$fixture" ] && [ -n "$input_sha" ] && [ -n "$codec" ] && \
        [ -n "$profile" ] && [ -n "$geometry" ] && [ -n "$field" ] && \
        [ -n "$frames" ] || die "fixture, hash, codec, profile, geometry, field, and frames are required"
    case $fixture in /*) ;; *) die "fixture path must be absolute" ;; esac
    is_uint "$frames" 10000000 || die "--frames must be a positive integer"
    case $geometry in
        *x*)
            width=${geometry%x*}
            height=${geometry#*x}
            is_uint "$width" 8192 && is_uint "$height" 8192 || die "invalid --geometry"
            ;;
        *) die "invalid --geometry" ;;
    esac
    validate_fixture "$fixture" "$input_sha" "$codec" "$profile" "$geometry" "$field" "$frames" || exit 1
    {
        printf 'fixture=%s\n' "$fixture"
        printf 'sha256=%s\n' "$input_sha"
        printf 'codec=%s\nprofile=%s\ngeometry=%s\nfield=%s\nframes=%s\n' \
            "$codec" "$profile" "$geometry" "$field" "$frames"
    } > "$results/fixture.txt"
}

set_acceptance_label()
{
    acceptance=development
    if [ "$mode" = oracle ]; then
        acceptance=oracle-v1
    elif [ "$rc" = yes ]; then
        case $mode in
            soak)
                case $geometry in
                    1280x720) acceptance=soak-720p30-28800 ;;
                    1920x1080) acceptance=soak-1080p30-$seconds ;;
                esac
                ;;
            churn) acceptance=churn-1000 ;;
            round-robin) acceptance=round-robin-100 ;;
            controls) acceptance=controls-10 ;;
            reload-sanity) acceptance=reload-sanity ;;
        esac
    fi
}

prepare_oracle()
{
    [ -n "$manifest" ] || die "--manifest is required"
    [ -n "$fixture_root" ] || die "--fixture-root is required"
    case $manifest in /*) ;; *) die "manifest path must be absolute" ;; esac
    case $fixture_root in /*) ;; *) die "fixture root must be absolute" ;; esac
    [ -d "$fixture_root" ] && [ -r "$fixture_root" ] || \
        die "fixture root is not a readable directory: $fixture_root"
    fixture_root=$(realpath -- "$fixture_root") || die "could not resolve fixture root"
    [ "$fixture_root" != / ] || die "fixture root must not be the filesystem root"
    manifest=$(realpath -- "$manifest") || die "could not resolve oracle manifest"
    if [ "$rc" = yes ] || [ "$mode" = oracle ]; then
        case $manifest in
            "$repo_dir"/*) manifest_relative=${manifest#"$repo_dir"/} ;;
            *) die "release oracle must be a tracked file in the source revision" ;;
        esac
        git -C "$repo_dir" ls-files --error-unmatch -- "$manifest_relative" \
            >/dev/null 2>&1 || die "release oracle is not tracked: $manifest_relative"
    fi
    cp -- "$manifest" "$results/oracle.tsv"
    validate_manifest "$results/oracle.tsv" || exit 1
    if [ "$rc" = yes ] || [ "$mode" = oracle ]; then
        git -C "$repo_dir" show "$revision:$manifest_relative" | \
            cmp -s - "$results/oracle.tsv" || \
            die "release oracle does not match revision $revision:$manifest_relative"
    fi
    oracle_hash=$(sha256_file "$results/oracle.tsv") || die "could not hash copied oracle"
    printf '%s  %s\n' "$oracle_hash" oracle.tsv > "$results/oracle.sha256"
    : > "$results/resolved-oracle.tsv"

    tab=$(printf '\t')
    while IFS="$tab" read -r scope row_id key fixture_name sha expected_profile \
        expected_geometry expected_field expected_frames expected_metadata expected_pixel; do
        case $scope in ''|'#'*) continue ;; esac
        file=$(realpath -- "$fixture_root/$fixture_name") || \
            die "could not resolve oracle fixture: $fixture_name"
        case $file in "$fixture_root"/*) ;; *) die "oracle fixture escapes fixture root: $fixture_name" ;; esac
        expected_codec=$(codec_name "$key") || die "unsupported oracle codec: $key"
        validate_fixture "$file" "$sha" "$expected_codec" "$expected_profile" \
            "$expected_geometry" "$expected_field" "$expected_frames" || exit 1
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$scope" "$row_id" "$key" "$file" "$sha" "$expected_profile" \
            "$expected_geometry" "$expected_field" "$expected_frames" \
            "$expected_metadata" "$expected_pixel" >> "$results/resolved-oracle.tsv"
    done < "$results/oracle.tsv"
    resolved_rounds=$(awk -F '\t' '$1 == "round" { n++ } END { print n + 0 }' \
        "$results/resolved-oracle.tsv")
    [ "$resolved_rounds" -eq 5 ] || die "resolved oracle does not contain all five round rows"
}

run_soak()
{
    [ "$codec" = h264 ] || die "soak requires H.264"
    [ "$field" = progressive ] || die "soak requires progressive H.264"
    is_uint "$seconds" 28800 || die "--seconds must be 1..28800"
    [ $((seconds % 12)) -eq 0 ] || die "--seconds must be a multiple of 12"
    rate=$(probe_value r_frame_rate "$fixture") || die "could not read fixture frame rate"
    [ "$rate" = 30/1 ] || die "soak fixture must be exactly 30 fps"
    format=$("$FFPROBE" -v error -show_entries format=format_name \
        -of default=nw=1:nk=1 "$fixture")
    case $format in *mp4*) ;; *) die "soak fixture must be MP4" ;; esac
    [ "$frames" -eq $((seconds * 30)) ] || die "soak fixture must contain seconds * 30 frames"
    if [ "$audio" = yes ]; then
        audio_codec=$(audio_probe_value codec_name "$fixture") || die "could not read soak audio stream"
        [ "$audio_codec" = aac ] || die "audio soak fixture must contain AAC"
    fi
    if [ "$rc" = yes ]; then
        valid_rc_soak "$geometry" "$seconds" "$profile" "$audio" || \
            die "the Phase 1 RC soak requires High/AAC at 720p for 28800s or Full HD for 7200..14400s"
    fi
    [ -n "$timeout_seconds" ] || timeout_seconds=$((seconds + 60))
    is_uint "$timeout_seconds" 32400 || die "invalid --timeout"
    [ "$timeout_seconds" -ge "$seconds" ] || die "soak timeout cannot be shorter than duration"
    set -- "$plugin_dir/gstreamer-controls-test" "$fixture" --expect-geometry "$geometry" \
        --sustain "$seconds" --timeout "$timeout_seconds"
    [ "$audio" = no ] || set -- "$@" --audio
    run_timed soak "$((timeout_seconds + 30))" "$@" || exit 1
}

run_churn()
{
    case $codec in h264|mpeg2video|vc1|wmv3) ;; *) die "churn codec is not supported by the direct-library probe" ;; esac
    [ -n "$iterations" ] || iterations=1000
    [ -n "$timeout_seconds" ] || timeout_seconds=30
    is_uint "$iterations" 1000 || die "invalid --iterations"
    [ "$rc" = no ] || [ "$iterations" -eq 1000 ] || \
        die "the Phase 1 RC churn gate requires exactly 1000 iterations"
    is_uint "$timeout_seconds" 300 || die "invalid --timeout"
    wall=$((iterations * timeout_seconds + 60))
    run_timed churn "$wall" "$script_dir/library-drain-test" --hardware \
        "$fixture" "$frames" "$timeout_seconds" "$iterations" || exit 1
}

run_oracle_row()
{
    row_stage=$1
    key=$2
    file=$3
    expected_frames=$4
    expected_geometry=$5
    expected_field=$6
    expected_metadata=$7
    expected_pixel=$8
    row_timeout=$9

    case $key in
        h264)
            format=$("$FFPROBE" -v error -show_entries format=format_name \
                -of default=nw=1:nk=1 "$file")
            case $format in
                *mp4*) container=mp4 ;;
                h264) container=h264 ;;
                *) die "unsupported H.264 container: $format" ;;
            esac
            expected_epoch_field=$(epoch_field "$expected_field") || \
                die "unsupported H.264 field order in oracle: $expected_field"
            set -- "$plugin_dir/gstreamer-playback-test" "$file" "$expected_frames" \
                "$container" "$row_timeout"
            if [ -n "$expected_epoch_field" ]; then
                set -- "$@" --epochs "$expected_geometry:$expected_frames:$expected_epoch_field"
            fi
            [ "$mode" != oracle ] || set -- "$@" --seek
            run_timed "$row_stage" "$((row_timeout + 15))" "$@" || exit 1
            ;;
        mpeg2)
            expected_epoch_field=$(epoch_field "$expected_field") || \
                die "unsupported MPEG-2 field order in oracle: $expected_field"
            set -- "$plugin_dir/gstreamer-playback-test" "$file" "$expected_frames" \
                mpeg2 "$row_timeout"
            if [ -n "$expected_epoch_field" ]; then
                set -- "$@" --epochs "$expected_geometry:$expected_frames:$expected_epoch_field"
            fi
            [ "$mode" != oracle ] || set -- "$@" --seek
            run_timed "$row_stage" "$((row_timeout + 15))" "$@" || exit 1
            ;;
        vc1|wmv3|mpeg4)
            run_timed "$row_stage" "$row_timeout" \
                "$plugin_dir/gstreamer-codec-playback-test" "$file" "$expected_frames" || exit 1
            ;;
    esac
    log=$results/stages/$(printf '%04d-%s' "$stage_number" "$row_stage").log
    observed_metadata=$(extract_metadata_sha "$log")
    observed_pixel=$(extract_pixel_sha "$log")
    [ "$observed_metadata" = "$expected_metadata" ] || \
        die "$key metadata SHA-256 mismatch: ${observed_metadata:-missing}"
    [ "$observed_pixel" = "$expected_pixel" ] || \
        die "$key output SHA-256 mismatch: ${observed_pixel:-missing}"
}

run_oracle()
{
    [ -z "$fixture$input_sha$codec$profile$geometry$field$frames$seconds$iterations$cycles" ] || \
        die "oracle accepts a manifest instead of individual fixture options"
    [ "$rc" = no ] || die "oracle is a conformance run and does not accept --rc"
    [ -n "$timeout_seconds" ] || timeout_seconds=45
    is_uint "$timeout_seconds" 300 || die "invalid --timeout"
    prepare_oracle

    tab=$(printf '\t')
    while IFS="$tab" read -r scope row_id key file sha expected_profile \
        expected_geometry expected_field expected_frames expected_metadata expected_pixel; do
        run_oracle_row "oracle-$row_id" "$key" "$file" "$expected_frames" \
            "$expected_geometry" "$expected_field" "$expected_metadata" \
            "$expected_pixel" "$timeout_seconds"
    done < "$results/resolved-oracle.tsv"
}

run_round_robin()
{
    [ -z "$fixture$input_sha$codec$profile$geometry$field$frames$seconds$iterations" ] || \
        die "round-robin accepts a manifest instead of individual fixture options"
    [ -n "$cycles" ] || cycles=100
    [ -n "$timeout_seconds" ] || timeout_seconds=45
    is_uint "$cycles" 1000 || die "invalid --cycles"
    [ "$rc" = no ] || [ "$cycles" -eq 100 ] || \
        die "the Phase 1 RC round-robin gate requires exactly 100 cycles"
    is_uint "$timeout_seconds" 300 || die "invalid --timeout"
    prepare_oracle

    tab=$(printf '\t')
    cycle=1
    while [ "$cycle" -le "$cycles" ]; do
        while IFS="$tab" read -r scope row_id key file sha expected_profile \
            expected_geometry expected_field expected_frames expected_metadata expected_pixel; do
            [ "$scope" = round ] || continue
            stage_key=$(printf 'round-%03d-%s' "$cycle" "$row_id")
            run_oracle_row "$stage_key" "$key" "$file" "$expected_frames" \
                "$expected_geometry" "$expected_field" "$expected_metadata" \
                "$expected_pixel" "$timeout_seconds"
        done < "$results/resolved-oracle.tsv"
        say "round-robin cycle $cycle/$cycles passed"
        cycle=$((cycle + 1))
    done
    while IFS="$tab" read -r scope row_id rest; do
        [ "$scope" = round ] || continue
        check_resource_growth "$row_id" yes "$RESOURCE_TREND_SAMPLES" || exit 1
    done < "$results/resolved-oracle.tsv"
}

run_controls()
{
    [ "$codec" = h264 ] || die "controls requires H.264"
    [ "$field" = progressive ] || die "controls requires progressive H.264"
    [ "$frames" -eq 360 ] || die "controls requires the 360-frame barcode fixture"
    rate=$(probe_value r_frame_rate "$fixture") || die "could not read fixture frame rate"
    [ "$rate" = 30/1 ] || die "controls fixture must be exactly 30 fps"
    format=$("$FFPROBE" -v error -show_entries format=format_name \
        -of default=nw=1:nk=1 "$fixture")
    case $format in *mp4*) ;; *) die "controls fixture must be MP4" ;; esac
    if [ "$audio" = yes ]; then
        audio_codec=$(audio_probe_value codec_name "$fixture") || die "could not read controls audio stream"
        [ "$audio_codec" = aac ] || die "audio controls fixture must contain AAC"
    fi
    [ -n "$timeout_seconds" ] || timeout_seconds=120
    is_uint "$timeout_seconds" 600 || die "invalid --timeout"
    [ -n "$cycles" ] || cycles=10
    is_uint "$cycles" 100 || die "invalid --cycles"
    if [ "$rc" = yes ]; then
        valid_rc_controls "$geometry" "$profile" "$audio" "$skip_2x" "$cycles" || \
            die "the Phase 1 RC controls gate requires 10 High/AAC 720p cycles including 2x"
    fi
    control_cycle=1
    while [ "$control_cycle" -le "$cycles" ]; do
        set -- "$plugin_dir/gstreamer-controls-test" "$fixture" \
            --expect-geometry "$geometry" --timeout "$timeout_seconds"
        [ "$audio" = no ] || set -- "$@" --audio
        [ "$skip_2x" = no ] || set -- "$@" --skip-2x
        run_timed "controls-$(printf '%03d' "$control_cycle")" \
            "$((timeout_seconds + 30))" "$@" || exit 1
        control_cycle=$((control_cycle + 1))
    done
    run_timed controls-immediate-reopen "$((timeout_seconds + 30))" \
        "$plugin_dir/gstreamer-playback-test" "$fixture" "$frames" mp4 \
        "$timeout_seconds" --epochs "$geometry:$frames:p" || exit 1
}

run_reload_sanity()
{
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || die "reload-sanity needs root or non-interactive sudo"
        sudo -n true >/dev/null 2>&1 || die "reload-sanity needs root or non-interactive sudo"
    fi
    case $codec in h264|mpeg2video|vc1|wmv3) ;; *) die "reload-sanity codec is not supported by the direct-library probe" ;; esac
    [ -n "$timeout_seconds" ] || timeout_seconds=30
    is_uint "$timeout_seconds" 300 || die "invalid --timeout"
    run_timed reload-before "$((timeout_seconds + 30))" "$script_dir/library-drain-test" \
        --hardware "$fixture" "$frames" "$timeout_seconds" 1 || exit 1
    device_idle || exit 1
    reload_force_value=$(normalized_force_l0s \
        "$(cat /sys/module/crystalhd/parameters/force_l0s_off 2>/dev/null)") || \
        die "cannot preserve force_l0s_off across reload-sanity"
    reload_in_progress=yes
    {
        root_exec "$TIMEOUT" --foreground --kill-after=5 30 modprobe -r crystalhd
        root_exec "$TIMEOUT" --foreground --kill-after=5 30 \
            modprobe crystalhd force_l0s_off="$reload_force_value"
        udevadm settle --timeout=10
    } > "$results/reload.log" 2>&1 || {
        sed -n '1,240p' "$results/reload.log" >&2
        die "normal crystalhd unload/reload failed"
    }
    reload_in_progress=no
    [ -r "$DEVICE" ] && [ -w "$DEVICE" ] || die "$DEVICE did not return after reload"
    device_idle || exit 1
    run_timed reload-after "$((timeout_seconds + 30))" "$script_dir/library-drain-test" \
        --hardware "$fixture" "$frames" "$timeout_seconds" 1 || exit 1
}

evidence_value()
{
    evidence_key=$1
    evidence_file=$2
    awk -v key="$evidence_key" '
        index($0, key "=") == 1 {
          count++
          value=substr($0, length(key) + 2)
        }
        END {
          if (count != 1) exit 1
          print value
        }
    ' "$evidence_file"
}

validate_evidence_fingerprint()
{
    fingerprint_file=$1
    fingerprint_revision=$2
    fingerprint_runtime=$3
    awk -F '\t' -v revision="$fingerprint_revision" \
        -v runtime="$fingerprint_runtime" '
        function sha(value) {
          return length(value) == 64 && value ~ /^[0-9a-f]+$/ && value !~ /^0+$/
        }
        function absolute(value) { return value ~ /^\// }
        {
          if ($1 == "git_revision") {
            revision_count++
            if (NF != 2 || $2 != revision) bad=1
          } else if ($1 == "runtime") {
            runtime_count++
            if (NF != 2 || $2 != runtime) bad=1
          } else if ($1 == "os_release") {
            os_count++
            if (NF != 2 || !sha($2)) bad=1
          } else if ($1 == "plugin") {
            plugin_count++
            if (NF != 3 || !absolute($2) || !sha($3)) bad=1
          } else if ($1 == "library") {
            library_count++
            if (NF != 3 || !absolute($2) || !sha($3)) bad=1
          } else if ($1 == "loaded_module_srcversion") {
            loaded_count++
            loaded=$2
            if (NF != 2 || $2 !~ /^[0-9A-Fa-f]+$/) bad=1
          } else if ($1 == "selected_module") {
            selected_count++
            if (NF != 2 || !absolute($2)) bad=1
          } else if ($1 == "selected_module_sha256") {
            selected_sha_count++
            if (NF != 2 || !sha($2)) bad=1
          } else if ($1 == "selected_module_srcversion") {
            selected_src_count++
            selected_src=$2
            if (NF != 2 || $2 !~ /^[0-9A-Fa-f]+$/) bad=1
          } else if ($1 == "selected_module_vermagic") {
            selected_vermagic_count++
            if (NF != 2 || $2 == "") bad=1
          } else if ($1 == "selected_module_build_note_sha256") {
            selected_note_count++
            selected_note=$2
            if (NF != 2 || !sha($2)) bad=1
          } else if ($1 == "loaded_module_build_note_sha256") {
            loaded_note_count++
            loaded_note=$2
            if (NF != 2 || !sha($2)) bad=1
          } else if ($1 == "firmware") {
            if (NF != 3 || !sha($3)) bad=1
            if ($2 == "/lib/firmware/bcm70015fw.bin") firmware_15++
            else if ($2 == "/lib/firmware/bcm70012fw.bin") firmware_12++
            else bad=1
          } else if ($1 == "probe") {
            if (NF != 3 || !absolute($2) || !sha($3)) bad=1
            if ($2 ~ /\/library-drain-test$/) drain++
            else if ($2 ~ /\/gstreamer-playback-test$/) playback++
            else if ($2 ~ /\/gstreamer-codec-playback-test$/) codec++
            else if ($2 ~ /\/gstreamer-controls-test$/) controls++
            else bad=1
          } else if ($1 == "tool") {
            if (NF != 3 || !absolute($2) || !sha($3)) bad=1
            if ($2 ~ /\/ffprobe$/) tool_ffprobe++
            else if ($2 ~ /\/sha256sum$/) tool_sha256sum++
            else if ($2 ~ /\/(gnu)?timeout$/) tool_timeout++
            else if ($2 ~ /\/fuser$/) tool_fuser++
            else if ($2 ~ /\/objcopy$/) tool_objcopy++
            else if ($2 ~ /\/zstdcat$/) tool_zstdcat++
            else bad=1
          } else {
            bad=1
          }
        }
        END {
          if (revision_count != 1 || runtime_count != 1 || os_count != 1 ||
              plugin_count != 1 || library_count != 1 || loaded_count != 1 ||
              selected_count != 1 || selected_sha_count != 1 ||
              selected_src_count != 1 || selected_vermagic_count != 1 ||
              selected_note_count != 1 || loaded_note_count != 1 ||
              firmware_15 != 1 || firmware_12 > 1 ||
              drain != 1 || playback != 1 || codec != 1 || controls != 1 ||
              tool_ffprobe != 1 || tool_sha256sum != 1 ||
              tool_timeout != 1 || tool_fuser != 1 || tool_objcopy != 1 ||
              tool_zstdcat != 1 || loaded != selected_src ||
              loaded_note != selected_note || bad) exit 1
          print loaded
        }
    ' "$fingerprint_file"
}

validate_evidence_identity()
{
    identity_file=$1
    identity_revision=$2
    identity_runtime=$3
    identity_mode=$4
    identity_rc=$5
    identity_acceptance=$6

    identity_started=$(evidence_value started "$identity_file") || return 1
    identity_arguments=$(evidence_value arguments "$identity_file") || return 1
    identity_source=$(evidence_value source "$identity_file") || return 1
    identity_kernel=$(evidence_value kernel "$identity_file") || return 1
    identity_os=$(evidence_value os_release_sha256 "$identity_file") || return 1
    identity_timeout=$(evidence_value timeout "$identity_file") || return 1
    identity_ffprobe=$(evidence_value ffprobe "$identity_file") || return 1
    identity_gstreamer=$(evidence_value gstreamer "$identity_file") || return 1
    identity_script=$(evidence_value script_sha256 "$identity_file") || return 1
    identity_module_src=$(evidence_value source_module_srcversion "$identity_file") || return 1
    identity_module_sha=$(evidence_value source_module_sha256 "$identity_file") || return 1
    identity_bdf=$(evidence_value pci_bdf "$identity_file") || return 1
    identity_vendor=$(evidence_value pci_vendor "$identity_file") || return 1
    identity_device=$(evidence_value pci_device "$identity_file") || return 1
    identity_subvendor=$(evidence_value pci_subsystem_vendor "$identity_file") || return 1
    identity_subdevice=$(evidence_value pci_subsystem_device "$identity_file") || return 1
    identity_pci_revision=$(evidence_value pci_revision "$identity_file") || return 1
    identity_class=$(evidence_value pci_class "$identity_file") || return 1

    [ -n "$identity_started" ] && [ -n "$identity_arguments" ] && \
        [ -n "$identity_kernel" ] && [ -n "$identity_timeout" ] && \
        [ -n "$identity_ffprobe" ] && [ -n "$identity_gstreamer" ] || return 1
    case $identity_source in /*) ;; *) return 1 ;; esac
    is_sha256 "$identity_os" && is_sha256 "$identity_script" && \
        is_sha256 "$identity_module_sha" || return 1
    printf '%s\n' "$identity_module_src" | grep -Eq '^[0-9A-Fa-f]+$' || return 1
    printf '%s\n' "$identity_bdf" | \
        grep -Eq '^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-7]$' || return 1
    [ "$identity_vendor" = 0x14e4 ] && [ "$identity_device" = 0x1615 ] || return 1
    printf '%s\n' "$identity_subvendor" | grep -Eq '^0x[0-9A-Fa-f]{4}$' || return 1
    printf '%s\n' "$identity_subdevice" | grep -Eq '^0x[0-9A-Fa-f]{4}$' || return 1
    printf '%s\n' "$identity_pci_revision" | grep -Eq '^0x[0-9A-Fa-f]{2}$' || return 1
    printf '%s\n' "$identity_class" | grep -Eq '^0x[0-9A-Fa-f]{6}$' || return 1
    identity_recorded_revision=$(evidence_value revision "$identity_file") || return 1
    identity_recorded_runtime=$(evidence_value runtime "$identity_file") || return 1
    identity_recorded_mode=$(evidence_value mode "$identity_file") || return 1
    identity_recorded_rc=$(evidence_value rc "$identity_file") || return 1
    identity_recorded_acceptance=$(evidence_value acceptance "$identity_file") || return 1
    [ "$identity_recorded_revision" = "$identity_revision" ] && \
        [ "$identity_recorded_runtime" = "$identity_runtime" ] && \
        [ "$identity_recorded_mode" = "$identity_mode" ] && \
        [ "$identity_recorded_rc" = "$identity_rc" ] && \
        [ "$identity_recorded_acceptance" = "$identity_acceptance" ] || return 1
    git -C "$repo_dir" cat-file -e \
        "$identity_revision:tests/phase1-release-gate.sh" >/dev/null 2>&1 || return 1
    identity_script_tmp=$(mktemp "${TMPDIR:-/tmp}/crystalhd-phase1-script.XXXXXX") || return 1
    if ! git -C "$repo_dir" show \
        "$identity_revision:tests/phase1-release-gate.sh" > "$identity_script_tmp"; then
        rm -f -- "$identity_script_tmp"
        return 1
    fi
    identity_committed_script=$(sha256_file "$identity_script_tmp") || {
        rm -f -- "$identity_script_tmp"
        return 1
    }
    rm -f -- "$identity_script_tmp"
    [ "$identity_script" = "$identity_committed_script" ] || return 1

    validated_identity_module_src=$identity_module_src
    validated_identity_invariant=$(printf '%s\n' \
        "source=$identity_source" "kernel=$identity_kernel" \
        "os_release_sha256=$identity_os" "timeout=$identity_timeout" \
        "ffprobe=$identity_ffprobe" "gstreamer=$identity_gstreamer" \
        "script_sha256=$identity_script" \
        "source_module_srcversion=$identity_module_src" \
        "source_module_sha256=$identity_module_sha" "pci_bdf=$identity_bdf" \
        "pci_vendor=$identity_vendor" "pci_device=$identity_device" \
        "pci_subsystem_vendor=$identity_subvendor" \
        "pci_subsystem_device=$identity_subdevice" \
        "pci_revision=$identity_pci_revision" "pci_class=$identity_class")
}

validate_evidence_stages()
{
    stage_file=$1
    stage_mode=$2
    stage_manifest=$3
    awk -F '\t' -v mode="$stage_mode" -v manifest="$stage_manifest" \
        -v stages="$stage_file" '
        FILENAME == manifest {
          if ($0 ~ /^#/ || $0 == "") next
          all_count++
          all_id[all_count]=$2
          if ($1 == "round") { round_count++; round_id[round_count]=$2 }
          next
        }
        FILENAME == stages {
          if (FNR == 1) {
            if ($0 != "stage\twall-timeout-seconds\texit-status") bad=1
            next
          }
          count++
          if (NF != 3 || $2 !~ /^[1-9][0-9]*$/ || $3 != "0") bad=1
          if (mode == "oracle") expected=sprintf("%04d-oracle-%s", count, all_id[count])
          else if (mode == "soak") expected="0001-soak"
          else if (mode == "churn") expected="0001-churn"
          else if (mode == "controls") {
            if (count <= 10) expected=sprintf("%04d-controls-%03d", count, count)
            else expected="0011-controls-immediate-reopen"
          } else if (mode == "reload-sanity") {
            expected=count == 1 ? "0001-reload-before" : "0002-reload-after"
          } else if (mode == "round-robin") {
            cycle=int((count - 1) / round_count) + 1
            item=((count - 1) % round_count) + 1
            expected=sprintf("%04d-round-%03d-%s", count, cycle, round_id[item])
          } else bad=1
          if ($1 != expected) bad=1
        }
        END {
          expected_count = mode == "oracle" ? all_count :
              mode == "round-robin" ? round_count * 100 :
              mode == "controls" ? 11 : mode == "reload-sanity" ? 2 : 1
          if (all_count < 5 || round_count != 5 || count != expected_count || bad) exit 1
        }
    ' "$stage_manifest" "$stage_file"
}

validate_evidence_resources()
{
    resource_stages=$1
    resource_samples=$2
    resource_summary=$3
    resource_manifest=$4
    resource_mode=$5
    resource_rc=$6
    awk -F '\t' -v stages="$resource_stages" -v manifest="$resource_manifest" \
        -v resources="$resource_samples" -v summary="$resource_summary" \
        -v mode="$resource_mode" -v rc="$resource_rc" \
        -v startup_samples="$RESOURCE_STARTUP_SAMPLES" \
        -v trend_samples="$RESOURCE_TREND_SAMPLES" \
        -v rss_limit="$RSS_GROWTH_KB" '
        function has_suffix(stage, row, suffix_length) {
          suffix_length=length(row)+1
          return length(stage) > suffix_length &&
              substr(stage, length(stage)-suffix_length+1) == "-" row
        }
        function calculate(name, n, aggregate,    i, key, window,
            post_startup, trend_checked, first, last,
            early_rss, early_fd, early_thread, late_rss, late_fd, late_thread,
            max_rss, value_rss, value_fd, value_thread, trend) {
          post_startup=n-startup_samples
          if (post_startup < 0) post_startup=0
          trend_checked=(post_startup >= trend_samples)
          if (post_startup > 0) {
            first=startup_samples+1
            window=int(post_startup/10); if (window < 3) window=3
            if (!trend_checked) window=post_startup
          } else {
            first=1
            window=n
          }
          last=trend_checked ? n-window+1 : first
          early_rss=early_fd=early_thread=late_rss=late_fd=late_thread=0
          max_rss=0
          for (i=1; i<=n; i++) {
            key=name SUBSEP i
            value_rss = aggregate ? round_rss[key] : stage_rss[key]
            if (i == 1 || value_rss > max_rss) max_rss=value_rss
          }
          for (i=first; i<first+window; i++) {
            key=name SUBSEP i
            early_rss += aggregate ? round_rss[key] : stage_rss[key]
            early_fd += aggregate ? round_fd[key] : stage_fd[key]
            early_thread += aggregate ? round_thread[key] : stage_thread[key]
          }
          for (i=last; i<last+window; i++) {
            key=name SUBSEP i
            late_rss += aggregate ? round_rss[key] : stage_rss[key]
            late_fd += aggregate ? round_fd[key] : stage_fd[key]
            late_thread += aggregate ? round_thread[key] : stage_thread[key]
          }
          early_rss/=window; early_fd/=window; early_thread/=window
          late_rss/=window; late_fd/=window; late_thread/=window
          trend = trend_checked ? "checked" : "insufficient-short-stage"
          growth_failure = trend_checked &&
              (late_rss > early_rss + rss_limit ||
               late_fd > early_fd + 8 || late_thread > early_thread + 4)
          return sprintf("%s\tsamples=%d\tRSS-early-avg=%.0f\tRSS-late-avg=%.0f\tRSS-max=%d\tFD-early-avg=%.1f\tFD-late-avg=%.1f\tthreads-early-avg=%.1f\tthreads-late-avg=%.1f\ttrend=%s",
              name, n, early_rss, late_rss, max_rss, early_fd, late_fd,
              early_thread, late_thread, trend)
        }
        FILENAME == stages {
          if (FNR == 1) next
          known[$1]=1
          next
        }
        FILENAME == manifest {
          if ($0 !~ /^#/ && $1 == "round") round[$2]=1
          next
        }
        FILENAME == resources {
          if (FNR == 1) {
            if ($0 != "stage\tsample\tepoch\trss-kb\tfds\tthreads\tprocesses") bad=1
            next
          }
          if (NF != 7 || !($1 in known) || $2 !~ /^[1-9][0-9]*$/ ||
              $3 !~ /^[0-9]+$/ || $4 !~ /^[0-9]+$/ || $5 !~ /^[0-9]+$/ ||
              $6 !~ /^[0-9]+$/ || $7 !~ /^[1-9][0-9]*$/ ||
              $6+0 < $7+0) {
            bad=1
            next
          }
          stage=$1
          if (($2+0) != stage_count[stage]+1) bad=1
          stage_count[stage]++
          key=stage SUBSEP stage_count[stage]
          stage_rss[key]=$4+0; stage_fd[key]=$5+0; stage_thread[key]=$6+0
          if (mode == "round-robin") {
            for (row in round) {
              if (!has_suffix(stage, row)) continue
              round_count[row]++
              key=row SUBSEP round_count[row]
              round_rss[key]=$4+0; round_fd[key]=$5+0; round_thread[key]=$6+0
            }
          }
          next
        }
        FILENAME == summary {
          if (FNR == 1) {
            if ($0 != "stage\tresource-summary") bad=1
            next
          }
          valid_name=($1 in known) ||
              (mode == "round-robin" && ($1 in round))
          if (NF != 10 || !valid_name || seen[$1]++ ||
              $2 !~ /^samples=[1-9][0-9]*$/ ||
              $3 !~ /^RSS-early-avg=[0-9]+$/ ||
              $4 !~ /^RSS-late-avg=[0-9]+$/ ||
              $5 !~ /^RSS-max=[0-9]+$/ ||
              $6 !~ /^FD-early-avg=[0-9]+\.[0-9]$/ ||
              $7 !~ /^FD-late-avg=[0-9]+\.[0-9]$/ ||
              $8 !~ /^threads-early-avg=[0-9]+\.[0-9]$/ ||
              $9 !~ /^threads-late-avg=[0-9]+\.[0-9]$/ ||
              $10 !~ /^trend=(checked|insufficient-short-stage)$/) bad=1
          supplied[$1]=$0
        }
        END {
          for (stage in known) {
            count=stage_count[stage]
            if (count == 0) { bad=1; continue }
            if (rc == "yes" && (mode == "soak" || mode == "churn") &&
                count-startup_samples < trend_samples) bad=1
            growth_failure=0
            expected=calculate(stage, count, 0)
            if (growth_failure || seen[stage] != 1 || supplied[stage] != expected) bad=1
          }
          if (mode == "round-robin") {
            for (row in round) {
              count=round_count[row]
              if (count < 100 || count-startup_samples < trend_samples) {
                bad=1
                continue
              }
              growth_failure=0
              expected=calculate(row, count, 1)
              if (growth_failure || seen[row] != 1 || supplied[row] != expected) bad=1
            }
          }
          if (bad) exit 1
        }
    ' "$resource_stages" "$resource_manifest" "$resource_samples" \
        "$resource_summary"
}

validate_evidence_fixture()
{
    fixture_file=$1
    fixture_evidence_name=$2
    fixture_acceptance=$3
    awk '
        /^fixture=/ { fixture++; next }
        /^sha256=/ { sha++; next }
        /^codec=/ { codec++; next }
        /^profile=/ { profile++; next }
        /^geometry=/ { geometry++; next }
        /^field=/ { field++; next }
        /^frames=/ { frames++; next }
        { bad=1 }
        END {
          if (fixture != 1 || sha != 1 || codec != 1 || profile != 1 ||
              geometry != 1 || field != 1 || frames != 1 || bad) exit 1
        }
    ' "$fixture_file" || return 1

    fixture_path=$(evidence_value fixture "$fixture_file") || return 1
    fixture_sha=$(evidence_value sha256 "$fixture_file") || return 1
    fixture_codec=$(evidence_value codec "$fixture_file") || return 1
    fixture_profile=$(evidence_value profile "$fixture_file") || return 1
    fixture_geometry=$(evidence_value geometry "$fixture_file") || return 1
    fixture_field=$(evidence_value field "$fixture_file") || return 1
    fixture_frames=$(evidence_value frames "$fixture_file") || return 1
    case $fixture_path in /*) ;; *) return 1 ;; esac
    is_sha256 "$fixture_sha" && [ -n "$fixture_profile" ] && \
        [ "$fixture_profile" != - ] || return 1
    printf '%s\n' "$fixture_sha" | grep -Eqv '^0+$' || return 1
    case $fixture_field in progressive|unknown|tt|tb|bb|bt) ;; *) return 1 ;; esac
    case $fixture_geometry in
        *x*)
            fixture_width=${fixture_geometry%x*}
            fixture_height=${fixture_geometry#*x}
            is_uint "$fixture_width" 8192 && is_uint "$fixture_height" 8192 || return 1
            ;;
        *) return 1 ;;
    esac
    is_uint "$fixture_frames" 10000000 || return 1

    case $fixture_evidence_name in
        soak-720p)
            [ "$fixture_codec" = h264 ] && [ "$fixture_profile" = High ] && \
                [ "$fixture_geometry" = 1280x720 ] && \
                [ "$fixture_field" = progressive ] && \
                [ "$fixture_frames" -eq 864000 ] || return 1
            ;;
        soak-1080p)
            fixture_seconds=${fixture_acceptance##*-}
            [ "$fixture_codec" = h264 ] && [ "$fixture_profile" = High ] && \
                [ "$fixture_geometry" = 1920x1080 ] && \
                [ "$fixture_field" = progressive ] && \
                [ "$fixture_frames" -eq $((fixture_seconds * 30)) ] || return 1
            ;;
        controls)
            [ "$fixture_codec" = h264 ] && [ "$fixture_profile" = High ] && \
                [ "$fixture_geometry" = 1280x720 ] && \
                [ "$fixture_field" = progressive ] && \
                [ "$fixture_frames" -eq 360 ] || return 1
            ;;
        churn|reload-sanity)
            case $fixture_codec in h264|mpeg2video|vc1|wmv3) ;; *) return 1 ;; esac
            [ "$fixture_width" -le 1920 ] && [ "$fixture_height" -le 1088 ] && \
                [ "$fixture_frames" -le 10000 ] || return 1
            case $fixture_field in progressive|unknown) ;; *) return 1 ;; esac
            ;;
        *) return 1 ;;
    esac
    validated_fixture_frames=$fixture_frames
}

validate_evidence_progress()
{
    progress_file=$1
    progress_mode=$2
    progress_stage=$3
    progress_manifest=$4
    progress_fixture_frames=$5
    progress_kind=
    progress_iteration=
    progress_pass=0

    case $progress_mode in
        oracle|round-robin)
            if [ "$progress_mode" = oracle ]; then
                progress_row=${progress_stage#????-oracle-}
            else
                progress_row=${progress_stage#????-round-???-}
            fi
            progress_row_data=$(awk -F '\t' -v id="$progress_row" \
                '$0 !~ /^#/ && $2 == id { print $3 "|" $9; found++ }
                 END { if (found != 1) exit 1 }' "$progress_manifest") || return 1
            progress_codec=${progress_row_data%%|*}
            progress_frames=${progress_row_data#*|}
            case $progress_codec in
                h264|mpeg2)
                    progress_kind=playback
                    [ "$progress_mode" != oracle ] || progress_pass=1
                    ;;
                vc1|wmv3|mpeg4) progress_kind=codec ;;
                *) return 1 ;;
            esac
            ;;
        soak)
            progress_kind=controls-soak
            progress_frames=$progress_fixture_frames
            ;;
        controls)
            progress_frames=$progress_fixture_frames
            case $progress_stage in
                *-controls-immediate-reopen) progress_kind=playback ;;
                *) progress_kind=controls ;;
            esac
            ;;
        churn)
            progress_kind=library
            progress_iteration=1000
            progress_frames=$progress_fixture_frames
            ;;
        reload-sanity)
            progress_kind=library
            progress_iteration=1
            progress_frames=$progress_fixture_frames
            ;;
        *) return 1 ;;
    esac
    is_uint "$progress_frames" 10000000 || return 1
    progress_last=$((progress_frames - 1))
    progress_line=$(tail -n 1 -- "$progress_file") || return 1
    printf '%s\n' "$progress_line" | awk -v kind="$progress_kind" \
        -v frame="$progress_last" -v frames="$progress_frames" \
        -v iteration="$progress_iteration" -v pass="$progress_pass" '
        function number(value) { return value ~ /^[0-9]+$/ }
        function field_value(field, prefix) {
          return index(field, prefix) == 1 ? substr(field, length(prefix) + 1) : ""
        }
        {
          if (kind == "playback") {
            valid=NF == 4 && $1 == "probe=gstreamer-playback" &&
                field_value($2, "pass=") == pass &&
                field_value($3, "frame-index=") == frame &&
                (field_value($4, "pts=") == "NONE" || number(field_value($4, "pts=")))
          } else if (kind == "codec") {
            valid=NF == 3 && $1 == "probe=gstreamer-codec-playback" &&
                field_value($2, "frame-index=") == frame &&
                (field_value($3, "pts=") == "NONE" || number(field_value($3, "pts=")))
          } else if (kind == "library") {
            valid=NF == 4 && $1 == "probe=library-drain" &&
                field_value($2, "iteration=") == iteration &&
                field_value($3, "frame-index=") == frame &&
                number(field_value($4, "token="))
          } else if (kind == "controls-soak") {
            valid=NF == 6 && $1 == "probe=gstreamer-controls" &&
                $2 == "phase=initial-1x" &&
                field_value($3, "frame-identity=") == frame &&
                field_value($4, "barcode=") == frame % 360 &&
                number(field_value($5, "pts=")) &&
                field_value($6, "total-video=") == frames
          } else if (kind == "controls") {
            total=field_value($6, "total-video=")
            valid=NF == 6 && $1 == "probe=gstreamer-controls" &&
                $2 == "phase=seek-0s-restored-1x" &&
                field_value($3, "frame-identity=") == 359 &&
                field_value($4, "barcode=") == 359 &&
                number(field_value($5, "pts=")) && number(total) && total >= 360
          }
        }
        END { exit !valid }
    '
}

validate_evidence_oracle_log()
{
    oracle_log_file=$1
    oracle_log_mode=$2
    oracle_log_stage=$3
    oracle_log_manifest=$4
    if [ "$oracle_log_mode" = oracle ]; then
        oracle_log_row=${oracle_log_stage#????-oracle-}
    else
        oracle_log_row=${oracle_log_stage#????-round-???-}
    fi
    oracle_log_data=$(awk -F '\t' -v id="$oracle_log_row" '
        $0 !~ /^#/ && $2 == id {
          print $3 "|" $6 "|" $9 "|" $10 "|" $11
          found++
        }
        END { if (found != 1) exit 1 }
    ' "$oracle_log_manifest") || return 1
    oracle_log_codec=${oracle_log_data%%|*}
    oracle_log_rest=${oracle_log_data#*|}
    oracle_log_profile=${oracle_log_rest%%|*}
    oracle_log_rest=${oracle_log_rest#*|}
    oracle_log_frames=${oracle_log_rest%%|*}
    oracle_log_rest=${oracle_log_rest#*|}
    oracle_log_metadata=${oracle_log_rest%%|*}
    oracle_log_pixels=${oracle_log_rest#*|}
    is_uint "$oracle_log_frames" 10000000 && \
        is_sha256 "$oracle_log_metadata" && is_sha256 "$oracle_log_pixels" || return 1

    case $oracle_log_codec in
        h264|mpeg2) oracle_log_kind=playback; oracle_log_identity= ;;
        vc1) oracle_log_kind=codec; oracle_log_identity=VC-1 ;;
        wmv3) oracle_log_kind=codec; oracle_log_identity=WMV3 ;;
        mpeg4)
            oracle_log_kind=codec
            case $oracle_log_profile in
                'Simple Profile') oracle_log_identity='MPEG-4 Simple' ;;
                'Advanced Simple Profile') oracle_log_identity='MPEG-4 ASP' ;;
                *) return 1 ;;
            esac
            ;;
        *) return 1 ;;
    esac

    awk -v kind="$oracle_log_kind" -v mode="$oracle_log_mode" \
        -v identity="$oracle_log_identity" -v frames="$oracle_log_frames" \
        -v metadata="$oracle_log_metadata" -v pixels="$oracle_log_pixels" '
        /MetadataSHA256=/ {
          summaries++
          suffix=frames "/" frames " YUY2 frames; EOS=yes; MetadataSHA256=" \
              metadata "; SHA256=" pixels
          if (kind == "playback") {
            if ($0 == "GStreamer decoded " suffix) plain++
            else if ($0 == "GStreamer seek replay decoded " suffix) replay++
            else bad=1
          } else {
            prefix=identity ": "
            packet_suffix=" packets; " suffix
            if (index($0, prefix) != 1) {
              bad=1
              next
            }
            body=substr($0, length(prefix) + 1)
            marker=index(body, packet_suffix)
            packets=marker ? substr(body, 1, marker - 1) : ""
            if (!marker || packets !~ /^[1-9][0-9]*$/ ||
                body != packets packet_suffix) bad=1
            else codec_summary++
          }
        }
        END {
          if (kind == "playback" && mode == "oracle")
            valid=summaries == 2 && plain == 1 && replay == 1
          else if (kind == "playback")
            valid=summaries == 1 && plain == 1 && replay == 0
          else
            valid=summaries == 1 && codec_summary == 1
          exit !(valid && !bad)
        }
    ' "$oracle_log_file"
}

tab_value()
{
    tab_key=$1
    tab_file=$2
    awk -F '\t' -v key="$tab_key" '
        $1 == key { count++; if (NF == 2) value=$2; else bad=1 }
        END { if (count != 1 || bad) exit 1; print value }
    ' "$tab_file"
}

validate_release_reload_evidence()
{
    reload_file=$1
    reload_fingerprint=$2
    reload_force=$3
    [ -f "$reload_file" ] || return 1
    reload_result=$(tab_value result "$reload_file") || return 1
    reload_path=$(tab_value selected_module "$reload_file") || return 1
    reload_hash=$(tab_value selected_module_sha256 "$reload_file") || return 1
    reload_src=$(tab_value selected_module_srcversion "$reload_file") || return 1
    reload_vermagic=$(tab_value selected_module_vermagic "$reload_file") || return 1
    reload_selected_note=$(tab_value selected_module_build_note_sha256 "$reload_file") || return 1
    reload_loaded_note=$(tab_value loaded_module_build_note_sha256 "$reload_file") || return 1
    reload_force_before=$(tab_value force_l0s_off_before "$reload_file") || return 1
    reload_force_after=$(tab_value force_l0s_off_after "$reload_file") || return 1
    reload_pci=$(tab_value pci "$reload_file") || return 1
    reload_firmware=$(awk -F '\t' '
        $1 == "firmware" && $2 == "/lib/firmware/bcm70015fw.bin" {
          count++; if (NF == 3) value=$3; else bad=1
        }
        END { if (count != 1 || bad) exit 1; print value }
    ' "$reload_file") || return 1
    reload_line_count=$(wc -l < "$reload_file") || return 1
    [ "$reload_line_count" -eq 11 ] && [ "$reload_result" = PASS ] && \
        [ "$reload_pci" = 14e4:1615 ] && [ "$reload_force_before" = "$reload_force" ] && \
        [ "$reload_force_after" = "$reload_force" ] && [ -n "$reload_vermagic" ] && \
        is_sha256 "$reload_hash" && is_sha256 "$reload_selected_note" && \
        is_sha256 "$reload_loaded_note" && is_sha256 "$reload_firmware" && \
        [ "$reload_selected_note" = "$reload_loaded_note" ] || return 1
    case $reload_path in /*) ;; *) return 1 ;; esac
    printf '%s\n' "$reload_src" | grep -Eq '^[0-9A-Fa-f]+$' || return 1

    [ "$reload_path" = "$(tab_value selected_module "$reload_fingerprint")" ] && \
        [ "$reload_hash" = "$(tab_value selected_module_sha256 "$reload_fingerprint")" ] && \
        [ "$reload_src" = "$(tab_value selected_module_srcversion "$reload_fingerprint")" ] && \
        [ "$reload_vermagic" = "$(tab_value selected_module_vermagic "$reload_fingerprint")" ] && \
        [ "$reload_selected_note" = "$(tab_value selected_module_build_note_sha256 "$reload_fingerprint")" ] && \
        [ "$reload_loaded_note" = "$(tab_value loaded_module_build_note_sha256 "$reload_fingerprint")" ] && \
        [ "$reload_firmware" = "$(awk -F '\t' '$1 == "firmware" && $2 == "/lib/firmware/bcm70015fw.bin" { print $3 }' "$reload_fingerprint")" ]
}

check_evidence_set()
{
    evidence_root=$1
    case $evidence_root in /*) ;; *) error "evidence root must be absolute"; return 1 ;; esac
    evidence_root=$(realpath "$evidence_root") || {
        error "could not resolve evidence root"
        return 1
    }
    [ -d "$evidence_root" ] || {
        error "evidence root is not a directory: $evidence_root"
        return 1
    }

    reference_revision=
    reference_runtime=
    reference_fingerprint=
    reference_identity=
    canonical_manifest=
    force_summary=
    for evidence_spec in \
        oracle:oracle:no:oracle-v1 \
        soak-720p:soak:yes:soak-720p30-28800 \
        soak-1080p:soak:yes:soak-1080p30 \
        churn:churn:yes:churn-1000 \
        round-robin:round-robin:yes:round-robin-100 \
        controls:controls:yes:controls-10 \
        reload-sanity:reload-sanity:yes:reload-sanity; do
        old_ifs=$IFS
        IFS=:
        set -- $evidence_spec
        IFS=$old_ifs
        evidence_name=$1
        expected_mode=$2
        expected_rc=$3
        expected_acceptance=$4
        evidence_dir=$evidence_root/$evidence_name
        result_file=$evidence_dir/result.txt
        [ -d "$evidence_dir" ] && [ -f "$result_file" ] || {
            error "missing evidence directory or result: $evidence_name"
            return 1
        }

        evidence_result=$(evidence_value result "$result_file") || {
            error "invalid result field in $evidence_name"
            return 1
        }
        evidence_revision=$(evidence_value revision "$result_file") || {
            error "invalid revision field in $evidence_name"
            return 1
        }
        evidence_runtime=$(evidence_value runtime "$result_file") || {
            error "invalid runtime field in $evidence_name"
            return 1
        }
        evidence_mode=$(evidence_value mode "$result_file") || {
            error "invalid mode field in $evidence_name"
            return 1
        }
        evidence_rc=$(evidence_value rc "$result_file") || {
            error "invalid rc field in $evidence_name"
            return 1
        }
        evidence_acceptance=$(evidence_value acceptance "$result_file") || {
            error "invalid acceptance field in $evidence_name"
            return 1
        }
        [ "$evidence_result" = PASS ] && [ "$evidence_mode" = "$expected_mode" ] && \
            [ "$evidence_rc" = "$expected_rc" ] || {
            error "wrong result, mode, or RC status in $evidence_name"
            return 1
        }
        if [ "$evidence_name" = soak-1080p ]; then
            case $evidence_acceptance in soak-1080p30-*) ;; *)
                error "wrong acceptance label in $evidence_name"
                return 1
            esac
            evidence_seconds=${evidence_acceptance##*-}
            is_uint "$evidence_seconds" 14400 && [ "$evidence_seconds" -ge 7200 ] || {
                error "Full-HD evidence is outside the 2-4 hour RC range"
                return 1
            }
        elif [ "$evidence_acceptance" != "$expected_acceptance" ]; then
            error "wrong acceptance label in $evidence_name"
            return 1
        fi
        case $evidence_revision in
            *[!0-9a-f]*|'') error "invalid revision in $evidence_name"; return 1 ;;
        esac
        [ "${#evidence_revision}" -eq 40 ] || {
            error "revision is not a full 40-character object name in $evidence_name"
            return 1
        }
        case $evidence_runtime in source|installed) ;; *)
            error "invalid runtime in $evidence_name"
            return 1
        esac

        identity_file=$evidence_dir/identity.txt
        if ! validate_evidence_identity "$identity_file" "$evidence_revision" \
            "$evidence_runtime" "$evidence_mode" "$evidence_rc" \
            "$evidence_acceptance"; then
            error "invalid or incomplete identity in $evidence_name"
            return 1
        fi
        evidence_identity=$validated_identity_invariant
        identity_module_src=$validated_identity_module_src

        fingerprint_before=$evidence_dir/runtime-fingerprint.before.tsv
        fingerprint_after=$evidence_dir/runtime-fingerprint.after.tsv
        [ -f "$fingerprint_before" ] && [ -f "$fingerprint_after" ] && \
            cmp -s -- "$fingerprint_before" "$fingerprint_after" || {
            error "runtime changed within $evidence_name"
            return 1
        }
        fingerprint_module_src=$(validate_evidence_fingerprint \
            "$fingerprint_before" "$evidence_revision" "$evidence_runtime") || {
            error "invalid or incomplete runtime fingerprint in $evidence_name"
            return 1
        }
        fingerprint_os=$(awk -F '\t' '$1 == "os_release" { print $2 }' \
            "$fingerprint_before")
        [ "$fingerprint_module_src" = "$identity_module_src" ] && \
            [ "$fingerprint_os" = "$identity_os" ] || {
            error "identity and runtime fingerprint disagree in $evidence_name"
            return 1
        }
        if [ -z "$reference_revision" ]; then
            reference_revision=$evidence_revision
            reference_runtime=$evidence_runtime
            reference_fingerprint=$fingerprint_before
            reference_identity=$evidence_identity
            git -C "$repo_dir" cat-file -e "$reference_revision^{commit}" \
                >/dev/null 2>&1 || {
                error "evidence revision is not a commit in this repository"
                return 1
            }
        else
            [ "$evidence_revision" = "$reference_revision" ] && \
                [ "$evidence_runtime" = "$reference_runtime" ] && \
                [ "$evidence_identity" = "$reference_identity" ] && \
                cmp -s -- "$reference_fingerprint" "$fingerprint_before" || {
                error "$evidence_name does not use the fixed candidate and machine identity"
                return 1
            }
        fi

        if [ "$evidence_name" = oracle ] || [ "$evidence_name" = round-robin ]; then
            evidence_manifest=$evidence_dir/oracle.tsv
            validate_manifest "$evidence_manifest" >/dev/null || {
                error "invalid oracle manifest in $evidence_name"
                return 1
            }
            git -C "$repo_dir" show \
                "$evidence_revision:tests/phase1-oracle.tsv" | \
                cmp -s - "$evidence_manifest" || {
                error "$evidence_name oracle is not the one committed in the candidate revision"
                return 1
            }
            if [ -z "$canonical_manifest" ]; then
                canonical_manifest=$evidence_manifest
            elif ! cmp -s -- "$canonical_manifest" "$evidence_manifest"; then
                error "oracle and round-robin did not use the same manifest"
                return 1
            fi
        fi
        [ -n "$canonical_manifest" ] || {
            error "canonical oracle was not established before $evidence_name"
            return 1
        }
        evidence_fixture_frames=
        if [ "$evidence_name" != oracle ] && [ "$evidence_name" != round-robin ]; then
            validate_evidence_fixture "$evidence_dir/fixture.txt" "$evidence_name" \
                "$evidence_acceptance" || {
                error "invalid or inconsistent fixture identity in $evidence_name"
                return 1
            }
            evidence_fixture_frames=$validated_fixture_frames
        fi

        system_before=$evidence_dir/system-before.txt
        system_after=$evidence_dir/system-after.txt
        before_ref=$(evidence_value module_refcount "$system_before" 2>/dev/null || true)
        after_ref=$(evidence_value module_refcount "$system_after" 2>/dev/null || true)
        before_pin=$(evidence_value pin_net "$system_before" 2>/dev/null || true)
        after_pin=$(evidence_value pin_net "$system_after" 2>/dev/null || true)
        before_src=$(evidence_value loaded_srcversion "$system_before" 2>/dev/null || true)
        after_src=$(evidence_value loaded_srcversion "$system_after" 2>/dev/null || true)
        before_force=$(evidence_value force_l0s_off "$system_before" 2>/dev/null || true)
        after_force=$(evidence_value force_l0s_off "$system_after" 2>/dev/null || true)
        [ "$before_ref" = 0 ] && [ "$after_ref" = 0 ] && \
            printf '%s\n' "$before_pin" | grep -Eq '^[0-9]+$' && \
            [ "$before_pin" = "$after_pin" ] && \
            [ "$before_src" = "$fingerprint_module_src" ] && \
            [ "$after_src" = "$fingerprint_module_src" ] && \
            printf '%s\n' "$before_force" | grep -Eq '^[YN]$' && \
            [ "$before_force" = "$after_force" ] || {
            error "resource or force_l0s_off state is inconsistent in $evidence_name"
            return 1
        }
        force_summary="$force_summary $evidence_name=$before_force"

        [ -f "$evidence_dir/module-reload.log" ] && \
            validate_release_reload_evidence "$evidence_dir/module-reload.tsv" \
                "$fingerprint_before" "$before_force" || {
            error "missing or inconsistent controlled reload evidence in $evidence_name"
            return 1
        }

        [ -f "$evidence_dir/aer.diff" ] && [ ! -s "$evidence_dir/aer.diff" ] && \
            printf '%s\n' 'kernel-log validation passed: no matching errors during the test' | \
                cmp -s - "$evidence_dir/kernel-check.log" || {
            error "missing clean AER/kernel evidence in $evidence_name"
            return 1
        }
        stage_file=$evidence_dir/stages.tsv
        validate_evidence_stages "$stage_file" "$evidence_mode" \
            "$canonical_manifest" || {
            error "invalid stage names, order, timeout, or result in $evidence_name"
            return 1
        }
        tab=$(printf '\t')
        while IFS="$tab" read -r evidence_stage evidence_wall evidence_status; do
            [ "$evidence_stage" != stage ] || continue
            [ -s "$evidence_dir/stages/$evidence_stage.log" ] && \
                [ -s "$evidence_dir/stages/$evidence_stage.progress" ] || {
                error "missing stage log or progress evidence for $evidence_name/$evidence_stage"
                return 1
            }
            validate_evidence_progress \
                "$evidence_dir/stages/$evidence_stage.progress" "$evidence_mode" \
                "$evidence_stage" "$canonical_manifest" \
                "$evidence_fixture_frames" || {
                error "invalid final probe progress for $evidence_name/$evidence_stage"
                return 1
            }
            case $evidence_mode in
                oracle|round-robin)
                    validate_evidence_oracle_log \
                        "$evidence_dir/stages/$evidence_stage.log" \
                        "$evidence_mode" "$evidence_stage" \
                        "$canonical_manifest" || {
                        error "oracle output identity or hashes do not match for $evidence_name/$evidence_stage"
                        return 1
                    }
                    ;;
            esac
        done < "$stage_file"
        [ -s "$evidence_dir/resources.tsv" ] && \
            [ -s "$evidence_dir/resource-summary.tsv" ] && \
            validate_evidence_resources "$stage_file" "$evidence_dir/resources.tsv" \
                "$evidence_dir/resource-summary.tsv" "$canonical_manifest" \
                "$evidence_mode" "$evidence_rc" || {
            error "invalid or incomplete resource evidence in $evidence_name"
            return 1
        }
    done
    say "Phase 1 evidence set passed: revision=$reference_revision runtime=$reference_runtime"
    say "force_l0s_off:$force_summary"
}

self_test()
{
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-phase1-selftest.XXXXXX")
    trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
    fixture=$tmp/fixture.mp4
    printf '%s\n' mock-fixture > "$fixture"
    sha=$(sha256_file "$fixture")
    mock_ffprobe=$tmp/ffprobe
    cat > "$mock_ffprobe" <<'EOF'
#!/bin/sh
case "$*" in
  *stream=codec_name*) printf '%s\n' h264 ;;
  *stream=profile*) printf '%s\n' High ;;
  *stream=width*) printf '%s\n' 1280 ;;
  *stream=height*) printf '%s\n' 720 ;;
  *stream=field_order*) printf '%s\n' progressive ;;
  *stream=nb_frames*) printf '%s\n' 360 ;;
  *stream=nb_read_frames*) printf '%s\n' 360 ;;
  *stream=r_frame_rate*) printf '%s\n' 30/1 ;;
  *format=format_name*) printf '%s\n' mov,mp4,m4a,3gp,3g2,mj2 ;;
  *-version*) printf '%s\n' 'mock ffprobe 1' ;;
  *) exit 2 ;;
esac
EOF
    chmod +x "$mock_ffprobe"
    FFPROBE=$mock_ffprobe
    tests=0

    saved_proc_root=$PROC_ROOT
    PROC_ROOT=$tmp/proc
    mkdir -p "$PROC_ROOT/100/fd" "$PROC_ROOT/101/fd" \
        "$PROC_ROOT/102/fd" "$PROC_ROOT/103/fd" "$PROC_ROOT/104"
    printf 'Name:\ttest\nState:\tR (running)\nVmRSS:\t100 kB\nThreads:\t2\n' \
        > "$PROC_ROOT/100/status"
    printf 'Name:\ttest\nState:\tS (sleeping)\nVmRSS:\t200 kB\nThreads:\t3\n' \
        > "$PROC_ROOT/101/status"
    printf 'Name:\ttest\nState:\tR (running)\nThreads:\t1\n' \
        > "$PROC_ROOT/102/status"
    printf 'Name:\ttest\nState:\tZ (zombie)\nVmRSS:\t999 kB\nThreads:\t1\n' \
        > "$PROC_ROOT/103/status"
    printf 'Name:\ttest\nState:\tR (running)\nVmRSS:\t400 kB\nThreads:\t4\n' \
        > "$PROC_ROOT/104/status"
    : > "$PROC_ROOT/100/fd/0"
    : > "$PROC_ROOT/100/fd/1"
    : > "$PROC_ROOT/101/fd/0"
    sleep 10 &
    policy_pid=$!
    mkdir "$PROC_ROOT/$policy_pid"
    printf 'Name:\ttest\nState:\tR (running)\n' \
        > "$PROC_ROOT/$policy_pid/status"
    policy_error=
    watchdog_is_live "$policy_pid" || policy_error=live
    printf 'Name:\ttest\nState:\tZ (zombie)\n' \
        > "$PROC_ROOT/$policy_pid/status"
    if watchdog_is_live "$policy_pid"; then
        policy_error=zombie
    fi
    kill -TERM "$policy_pid" 2>/dev/null || true
    wait "$policy_pid" 2>/dev/null || true
    [ -z "$policy_error" ] || {
        error "self-test disagreed with the watchdog live/zombie policy"
        return 1
    }
    process_results=$tmp/process-results
    mkdir "$process_results"
    results=$process_results
    printf 'stage\tsample\tepoch\trss-kb\tfds\tthreads\tprocesses\n' \
        > "$results/resources.tsv"
    sample_process_list tree 1 100 101 999
    awk -F '\t' '$1 == "tree" && $2 == 1 && $4 == 300 && $5 == 3 && \
        $6 == 5 && $7 == 2 { found=1 } END { exit !found }' \
        "$results/resources.tsv" || {
        error "self-test did not record the complete live process set"
        return 1
    }
    sample_process_list zombie 1 100 103
    awk -F '\t' '$1 == "zombie" && $4 == 100 && $5 == 2 && $6 == 2 && \
        $7 == 1 { found=1 } END { exit !found }' \
        "$results/resources.tsv" || {
        error "self-test did not skip an exited zombie"
        return 1
    }
    process_lines=$(wc -l < "$results/resources.tsv")
    if sample_process_list gone 1 999 >/dev/null 2>&1 || \
        sample_process_list incomplete 1 100 102 >/dev/null 2>&1 || \
        sample_process_list no-fds 1 100 104 >/dev/null 2>&1; then
        error "self-test accepted a missing or incomplete live process sample"
        return 1
    fi
    [ "$(wc -l < "$results/resources.tsv")" -eq "$process_lines" ] || {
        error "self-test emitted a partial or zero-process resource row"
        return 1
    }
    PROC_ROOT=$saved_proc_root
    tests=$((tests + 1))

    if ! watchdog_works "$TIMEOUT"; then
        error "self-test requires a watchdog that enforces TERM followed by KILL"
        return 1
    fi
    tests=$((tests + 1))
    if ! watchdog_result_valid 137 2 || watchdog_result_valid 137 0 || \
        watchdog_result_valid 137 10 || watchdog_result_valid 124 2; then
        error "self-test accepted an invalid watchdog status or elapsed time"
        return 1
    fi
    tests=$((tests + 1))

    [ "$(epoch_field progressive)" = p ] && [ "$(epoch_field tt)" = tff ] && \
        [ "$(epoch_field bt)" = tff ] && [ "$(epoch_field bb)" = bff ] && \
        [ "$(epoch_field tb)" = bff ] && [ -z "$(epoch_field unknown)" ]
    tests=$((tests + 1))

    valid_rc_soak 1280x720 28800 High yes && \
        valid_rc_soak 1920x1080 7200 High yes && \
        valid_rc_soak 1920x1080 14400 High yes && \
        ! valid_rc_soak 1280x720 28788 High yes && \
        ! valid_rc_soak 1920x1080 7199 High yes && \
        ! valid_rc_soak 640x360 28800 High yes && \
        ! valid_rc_soak 1280x720 28800 Main yes && \
        ! valid_rc_soak 1280x720 28800 High no
    tests=$((tests + 1))
    valid_rc_controls 1280x720 High yes no 10 && \
        ! valid_rc_controls 1920x1080 High yes no 10 && \
        ! valid_rc_controls 1280x720 High yes yes 10 && \
        ! valid_rc_controls 1280x720 High yes no 1
    tests=$((tests + 1))

    validate_fixture "$fixture" "$sha" h264 High 1280x720 progressive 360 >/dev/null
    tests=$((tests + 1))
    if validate_fixture "$fixture" "$(printf '%064d' 0)" h264 High 1280x720 progressive 360 >/dev/null 2>&1; then
        error "self-test accepted a wrong fixture hash"
        return 1
    fi
    tests=$((tests + 1))
    if validate_fixture "$fixture" "$sha" h264 Main 1280x720 progressive 360 >/dev/null 2>&1; then
        error "self-test accepted a wrong fixture profile"
        return 1
    fi
    tests=$((tests + 1))

    metadata_h264=$(printf '%064d' 1)
    metadata_mpeg2=$(printf '%064d' 2)
    metadata_vc1=$(printf '%064d' 3)
    metadata_wmv3=$(printf '%064d' 4)
    metadata_mpeg4=$(printf '%064d' 5)
    output_h264=$(printf '%064d' 11)
    output_mpeg2=$(printf '%064d' 12)
    output_vc1=$(printf '%064d' 13)
    output_wmv3=$(printf '%064d' 14)
    output_mpeg4=$(printf '%064d' 15)
    manifest=$tmp/oracle.tsv
    {
        printf '%s\n' '# crystalhd-oracle-v1 metadata=CHMD2 pixels=YUY2-active-row-v1'
        printf '# scope\tid\tcodec\tfixture\tinput\tprofile\tgeometry\tfield\tframes\tmetadata\tpixels\n'
        printf 'round\th264-short\th264\tfixture.mp4\t%s\tHigh\t1280x720\tprogressive\t360\t%s\t%s\n' "$sha" "$metadata_h264" "$output_h264"
        printf 'round\tmpeg2-short\tmpeg2\tfixture.mp4\t%s\tMain\t1280x720\tprogressive\t360\t%s\t%s\n' "$sha" "$metadata_mpeg2" "$output_mpeg2"
        printf 'round\tvc1-short\tvc1\tfixture.mp4\t%s\tAdvanced\t1280x720\tprogressive\t360\t%s\t%s\n' "$sha" "$metadata_vc1" "$output_vc1"
        printf 'round\twmv3-short\twmv3\tfixture.mp4\t%s\tMain\t1280x720\tunknown\t360\t%s\t%s\n' "$sha" "$metadata_wmv3" "$output_wmv3"
        printf 'round\tmpeg4-short\tmpeg4\tfixture.mp4\t%s\tAdvanced Simple Profile\t1280x720\tprogressive\t360\t%s\t%s\n' "$sha" "$metadata_mpeg4" "$output_mpeg4"
        printf 'conformance\th264-extra\th264\tfixture.mp4\t%s\tHigh\t1280x720\tprogressive\t360\t%s\t%s\n' "$sha" "$metadata_h264" "$output_h264"
    } > "$manifest"
    validate_manifest "$manifest" >/dev/null
    tests=$((tests + 1))
    sed 's/^round\tmpeg2-short\tmpeg2\t/round\tmpeg2-short\tmpeg4\t/' \
        "$manifest" > "$tmp/bad-order.tsv"
    if validate_manifest "$tmp/bad-order.tsv" >/dev/null 2>&1; then
        error "self-test accepted a wrong codec order"
        return 1
    fi
    tests=$((tests + 1))
    sed "s/$metadata_h264/$(printf '%064d' 0)/" "$manifest" > "$tmp/placeholder.tsv"
    if validate_manifest "$tmp/placeholder.tsv" >/dev/null 2>&1; then
        error "self-test accepted a placeholder output hash"
        return 1
    fi
    tests=$((tests + 1))
    sed 's/fixture\.mp4/\/fixture.mp4/' "$manifest" > "$tmp/absolute.tsv"
    if validate_manifest "$tmp/absolute.tsv" >/dev/null 2>&1; then
        error "self-test accepted an absolute fixture path"
        return 1
    fi
    tests=$((tests + 1))
    sed '$ s/$//' "$manifest" | head -c -1 > "$tmp/no-final-newline.tsv"
    if validate_manifest "$tmp/no-final-newline.tsv" >/dev/null 2>&1; then
        error "self-test accepted a manifest without a final newline"
        return 1
    fi
    tests=$((tests + 1))

    results=$tmp/results
    mkdir "$results"
    printf 'stage\tsample\tepoch\trss-kb\tfds\tthreads\tprocesses\n' > "$results/resources.tsv"
    printf 'stage\tresource-summary\n' > "$results/resource-summary.tsv"
    sample=1
    while [ "$sample" -le 16 ]; do
        printf 'stable\t%s\t%s\t1000\t4\t2\t1\n' "$sample" "$sample" >> "$results/resources.tsv"
        printf 'growing\t%s\t%s\t%s\t4\t2\t1\n' "$sample" "$sample" "$((sample * 40000))" >> "$results/resources.tsv"
        sample=$((sample + 1))
    done
    sample=0
    for value in 13344 15188 13492 13504 13504 81352 81352 81352 \
        81352 81352 77612 80000 80000 80000 80000 80000; do
        sample=$((sample + 1))
        printf 'plateau\t%s\t%s\t%s\t4\t2\t1\n' "$sample" "$sample" "$value" >> "$results/resources.tsv"
    done
    sample=0
    for value in 13344 15188 13492 13504 13504 81352 81352 81352 \
        81352 81352 77612; do
        sample=$((sample + 1))
        printf 'startup-short\t%s\t%s\t%s\t4\t2\t1\n' "$sample" "$sample" "$value" >> "$results/resources.tsv"
    done
    sample=0
    for value in 13344 15188 13492 13504 13504 81352 81352 81352 \
        81352 81352 81000 120000 119000 160000 159000 200000; do
        sample=$((sample + 1))
        printf 'sawtooth\t%s\t%s\t%s\t4\t2\t1\n' "$sample" "$sample" "$value" >> "$results/resources.tsv"
    done
    sample=1
    while [ "$sample" -le 15 ]; do
        printf 'rc-short\t%s\t%s\t1000\t4\t2\t1\n' \
            "$sample" "$sample" >> "$results/resources.tsv"
        sample=$((sample + 1))
    done
    check_resource_growth stable >/dev/null
    check_resource_growth plateau no "$RESOURCE_TREND_SAMPLES" >/dev/null
    tests=$((tests + 1))
    check_resource_growth startup-short >/dev/null
    tail -n 1 "$results/resource-summary.tsv" | \
        grep -q 'startup-short.*trend=insufficient-short-stage$' || {
        error "self-test treated a startup-only stage as a resource trend"
        return 1
    }
    tests=$((tests + 1))
    if check_resource_growth growing >/dev/null 2>&1; then
        error "self-test accepted monotonic RSS growth"
        return 1
    fi
    tests=$((tests + 1))
    if check_resource_growth sawtooth >/dev/null 2>&1; then
        error "self-test accepted sawtooth RSS growth"
        return 1
    fi
    tests=$((tests + 1))
    if check_resource_growth rc-short no "$RESOURCE_TREND_SAMPLES" \
        >/dev/null 2>&1; then
        error "self-test accepted too few post-startup samples for a long release stage"
        return 1
    fi
    tests=$((tests + 1))

    printf 'crystalhd 1 0 - Live 0x0\n' > "$tmp/modules"
    mock_fuser=$tmp/fuser
    cat > "$mock_fuser" <<'EOF'
#!/bin/sh
[ "${MOCK_BUSY:-no}" = yes ] && exit 0
exit 1
EOF
    chmod +x "$mock_fuser"
    MODULES_FILE=$tmp/modules
    FUSER=$mock_fuser
    DEVICE=$fixture
    device_idle >/dev/null
    tests=$((tests + 1))
    MOCK_BUSY=yes
    export MOCK_BUSY
    if device_idle >/dev/null 2>&1; then
        error "self-test accepted a busy device"
        return 1
    fi
    unset MOCK_BUSY
    tests=$((tests + 1))

    sha_log=$tmp/output.log
    printf 'probe: 360/360 frames; EOS=yes; MetadataSHA256=%s; SHA256=%s\n' \
        "$metadata_h264" "$output_h264" > "$sha_log"
    [ "$(extract_metadata_sha "$sha_log")" = "$metadata_h264" ] && \
        [ "$(extract_pixel_sha "$sha_log")" = "$output_h264" ] || {
        error "self-test failed to extract metadata or pixel SHA-256"
        return 1
    }
    tests=$((tests + 1))

    evidence_root=$tmp/evidence
    evidence_repo=$tmp/evidence-repo
    mkdir -p "$evidence_root" "$evidence_repo/tests"
    cp "$manifest" "$evidence_repo/tests/phase1-oracle.tsv"
    printf '%s\n' '#!/bin/sh' 'exit 0' > "$evidence_repo/tests/phase1-release-gate.sh"
    git -C "$evidence_repo" init -q
    git -C "$evidence_repo" add tests/phase1-oracle.tsv tests/phase1-release-gate.sh
    git -C "$evidence_repo" -c user.name='Phase 1 self-test' \
        -c user.email='phase1-self-test@example.invalid' commit -q -m evidence
    evidence_revision=$(git -C "$evidence_repo" rev-parse HEAD)
    evidence_hash=$(printf '%064d' 42)
    evidence_script_hash=$(sha256_file "$evidence_repo/tests/phase1-release-gate.sh")
    evidence_srcversion=ABCDEF12
    evidence_fingerprint=$tmp/fingerprint
    {
        printf 'git_revision\t%s\n' "$evidence_revision"
        printf 'runtime\tinstalled\n'
        printf 'os_release\t%s\n' "$evidence_hash"
        printf 'plugin\t/opt/crystalhd/libgstcrystalhd.so\t%s\n' "$evidence_hash"
        printf 'library\t/opt/crystalhd/libcrystalhd.so.3\t%s\n' "$evidence_hash"
        printf 'loaded_module_srcversion\t%s\n' "$evidence_srcversion"
        printf 'selected_module\t/opt/crystalhd/crystalhd.ko\n'
        printf 'selected_module_sha256\t%s\n' "$evidence_hash"
        printf 'selected_module_srcversion\t%s\n' "$evidence_srcversion"
        printf 'selected_module_vermagic\t6.17.0-test SMP mod_unload\n'
        printf 'selected_module_build_note_sha256\t%s\n' "$evidence_hash"
        printf 'loaded_module_build_note_sha256\t%s\n' "$evidence_hash"
        printf 'firmware\t/lib/firmware/bcm70015fw.bin\t%s\n' "$evidence_hash"
        printf 'probe\t/opt/crystalhd/library-drain-test\t%s\n' "$evidence_hash"
        printf 'probe\t/opt/crystalhd/gstreamer-playback-test\t%s\n' "$evidence_hash"
        printf 'probe\t/opt/crystalhd/gstreamer-codec-playback-test\t%s\n' "$evidence_hash"
        printf 'probe\t/opt/crystalhd/gstreamer-controls-test\t%s\n' "$evidence_hash"
        printf 'tool\t/usr/bin/ffprobe\t%s\n' "$evidence_hash"
        printf 'tool\t/usr/bin/sha256sum\t%s\n' "$evidence_hash"
        printf 'tool\t/usr/bin/timeout\t%s\n' "$evidence_hash"
        printf 'tool\t/usr/bin/fuser\t%s\n' "$evidence_hash"
        printf 'tool\t/usr/bin/objcopy\t%s\n' "$evidence_hash"
        printf 'tool\t/usr/bin/zstdcat\t%s\n' "$evidence_hash"
    } > "$evidence_fingerprint"

    mock_evidence_stage()
    {
        mock_stage_name=$1
        mock_progress_line=$2
        mock_samples=${3:-1}
        if [ "$#" -ge 4 ]; then
            mock_log_text=$4
        else
            mock_log_text="stage $mock_stage_name passed"
        fi
        printf '%s\t60\t0\n' "$mock_stage_name" >> "$evidence_dir/stages.tsv"
        printf '%s\n' "$mock_log_text" > \
            "$evidence_dir/stages/$mock_stage_name.log"
        printf '%s\n' "$mock_progress_line" > \
            "$evidence_dir/stages/$mock_stage_name.progress"
        mock_sample=1
        while [ "$mock_sample" -le "$mock_samples" ]; do
            printf '%s\t%s\t%s\t1000\t4\t2\t1\n' "$mock_stage_name" \
                "$mock_sample" "$mock_sample" >> "$evidence_dir/resources.tsv"
            mock_sample=$((mock_sample + 1))
        done
        if [ "$mock_samples" -ge $((RESOURCE_STARTUP_SAMPLES + RESOURCE_TREND_SAMPLES)) ]; then
            mock_trend=checked
        else
            mock_trend=insufficient-short-stage
        fi
        printf '%s\tsamples=%s\tRSS-early-avg=1000\tRSS-late-avg=1000\tRSS-max=1000\tFD-early-avg=4.0\tFD-late-avg=4.0\tthreads-early-avg=2.0\tthreads-late-avg=2.0\ttrend=%s\n' \
            "$mock_stage_name" "$mock_samples" "$mock_trend" >> \
            "$evidence_dir/resource-summary.tsv"
    }

    mock_oracle_log()
    {
        mock_log_codec=$1
        mock_log_profile=$2
        mock_log_frames=$3
        mock_log_metadata=$4
        mock_log_pixels=$5
        mock_log_mode=$6
        mock_log_suffix="$mock_log_frames/$mock_log_frames YUY2 frames; EOS=yes; MetadataSHA256=$mock_log_metadata; SHA256=$mock_log_pixels"
        case $mock_log_codec in
            h264|mpeg2)
                if [ "$mock_log_mode" = oracle ]; then
                    printf 'GStreamer decoded %s\nGStreamer seek replay decoded %s\n' \
                        "$mock_log_suffix" "$mock_log_suffix"
                else
                    printf 'GStreamer decoded %s\n' "$mock_log_suffix"
                fi
                ;;
            vc1) mock_log_identity=VC-1 ;;
            wmv3) mock_log_identity=WMV3 ;;
            mpeg4)
                case $mock_log_profile in
                    'Simple Profile') mock_log_identity='MPEG-4 Simple' ;;
                    'Advanced Simple Profile') mock_log_identity='MPEG-4 ASP' ;;
                    *) return 1 ;;
                esac
                ;;
            *) return 1 ;;
        esac
        case $mock_log_codec in
            h264|mpeg2) ;;
            *)
                printf '%s: %s packets; %s\n' "$mock_log_identity" \
                    "$mock_log_frames" "$mock_log_suffix"
                ;;
        esac
    }

    for evidence_spec in \
        oracle:oracle:no:oracle-v1 \
        soak-720p:soak:yes:soak-720p30-28800 \
        soak-1080p:soak:yes:soak-1080p30-7200 \
        churn:churn:yes:churn-1000 \
        round-robin:round-robin:yes:round-robin-100 \
        controls:controls:yes:controls-10 \
        reload-sanity:reload-sanity:yes:reload-sanity; do
        old_ifs=$IFS
        IFS=:
        set -- $evidence_spec
        IFS=$old_ifs
        evidence_name=$1
        evidence_mode=$2
        evidence_rc=$3
        evidence_acceptance=$4
        evidence_dir=$evidence_root/$evidence_name
        mkdir -p "$evidence_dir/stages"
        printf 'result=PASS\nrevision=%s\nruntime=installed\nmode=%s\nrc=%s\nacceptance=%s\n' \
            "$evidence_revision" "$evidence_mode" "$evidence_rc" \
            "$evidence_acceptance" > "$evidence_dir/result.txt"
        {
            printf 'started=2026-09-28 00:00:00.000000\n'
            printf 'mode=%s\narguments=mock %s\nsource=%s\nrevision=%s\n' \
                "$evidence_mode" "$evidence_name" "$evidence_repo" "$evidence_revision"
            printf 'runtime=installed\nrc=%s\nacceptance=%s\n' \
                "$evidence_rc" "$evidence_acceptance"
            printf 'kernel=Linux mock 6.1 x86_64\nos_release_sha256=%s\n' "$evidence_hash"
            printf 'timeout=timeout (GNU coreutils) mock\nffprobe=ffprobe mock\n'
            printf 'gstreamer=gst-inspect mock\nscript_sha256=%s\n' "$evidence_script_hash"
            printf 'source_module_srcversion=%s\nsource_module_sha256=%s\n' \
                "$evidence_srcversion" "$evidence_hash"
            printf 'pci_bdf=0000:02:00.0\npci_vendor=0x14e4\npci_device=0x1615\n'
            printf 'pci_subsystem_vendor=0x14e4\npci_subsystem_device=0x1615\n'
            printf 'pci_revision=0x01\npci_class=0x040000\n'
        } > "$evidence_dir/identity.txt"
        cp "$evidence_fingerprint" "$evidence_dir/runtime-fingerprint.before.tsv"
        cp "$evidence_fingerprint" "$evidence_dir/runtime-fingerprint.after.tsv"
        evidence_force=N
        [ "$evidence_name" != soak-1080p ] || evidence_force=Y
        printf 'module_refcount=0\npin_net=7\nloaded_srcversion=%s\nforce_l0s_off=%s\n' \
            "$evidence_srcversion" "$evidence_force" > "$evidence_dir/system-before.txt"
        cp "$evidence_dir/system-before.txt" "$evidence_dir/system-after.txt"
        : > "$evidence_dir/module-reload.log"
        {
            printf 'result\tPASS\n'
            printf 'selected_module\t/opt/crystalhd/crystalhd.ko\n'
            printf 'selected_module_sha256\t%s\n' "$evidence_hash"
            printf 'selected_module_srcversion\t%s\n' "$evidence_srcversion"
            printf 'selected_module_vermagic\t6.17.0-test SMP mod_unload\n'
            printf 'selected_module_build_note_sha256\t%s\n' "$evidence_hash"
            printf 'loaded_module_build_note_sha256\t%s\n' "$evidence_hash"
            printf 'firmware\t/lib/firmware/bcm70015fw.bin\t%s\n' "$evidence_hash"
            printf 'force_l0s_off_before\t%s\n' "$evidence_force"
            printf 'force_l0s_off_after\t%s\n' "$evidence_force"
            printf 'pci\t14e4:1615\n'
        } > "$evidence_dir/module-reload.tsv"
        : > "$evidence_dir/aer.diff"
        printf '%s\n' 'kernel-log validation passed: no matching errors during the test' \
            > "$evidence_dir/kernel-check.log"
        printf 'stage\twall-timeout-seconds\texit-status\n' > "$evidence_dir/stages.tsv"
        printf 'stage\tsample\tepoch\trss-kb\tfds\tthreads\tprocesses\n' \
            > "$evidence_dir/resources.tsv"
        printf 'stage\tresource-summary\n' > "$evidence_dir/resource-summary.tsv"
        case $evidence_name in
            soak-720p)
                fixture_codec=h264; fixture_profile=High; fixture_geometry=1280x720
                fixture_field=progressive; fixture_frames=864000
                ;;
            soak-1080p)
                fixture_codec=h264; fixture_profile=High; fixture_geometry=1920x1080
                fixture_field=progressive; fixture_frames=216000
                ;;
            controls)
                fixture_codec=h264; fixture_profile=High; fixture_geometry=1280x720
                fixture_field=progressive; fixture_frames=360
                ;;
            churn|reload-sanity)
                fixture_codec=h264; fixture_profile=High; fixture_geometry=1280x720
                fixture_field=progressive; fixture_frames=360
                ;;
            *) fixture_codec= ;;
        esac
        if [ -n "$fixture_codec" ]; then
            printf 'fixture=%s\nsha256=%s\ncodec=%s\nprofile=%s\ngeometry=%s\nfield=%s\nframes=%s\n' \
                "$fixture" "$sha" "$fixture_codec" "$fixture_profile" \
                "$fixture_geometry" "$fixture_field" "$fixture_frames" > \
                "$evidence_dir/fixture.txt"
        fi
        tab=$(printf '\t')
        evidence_stage=0
        case $evidence_mode in
            oracle)
                cp "$manifest" "$evidence_dir/oracle.tsv"
                while IFS="$tab" read -r scope row_id row_codec row_fixture row_sha \
                    row_profile row_geometry row_field row_frames row_metadata \
                    row_pixels; do
                    case $scope in ''|'#'*) continue ;; esac
                    evidence_stage=$((evidence_stage + 1))
                    row_last=$((row_frames - 1))
                    case $row_codec in
                        h264|mpeg2)
                            row_progress="probe=gstreamer-playback pass=1 frame-index=$row_last pts=0"
                            ;;
                        *)
                            row_progress="probe=gstreamer-codec-playback frame-index=$row_last pts=0"
                            ;;
                    esac
                    row_log=$(mock_oracle_log "$row_codec" "$row_profile" \
                        "$row_frames" "$row_metadata" "$row_pixels" oracle) || \
                        return 1
                    mock_evidence_stage \
                        "$(printf '%04d-oracle-%s' "$evidence_stage" "$row_id")" \
                        "$row_progress" 1 "$row_log"
                done < "$manifest"
                ;;
            round-robin)
                cp "$manifest" "$evidence_dir/oracle.tsv"
                evidence_cycle=1
                while [ "$evidence_cycle" -le 100 ]; do
                    while IFS="$tab" read -r scope row_id row_codec row_fixture row_sha \
                        row_profile row_geometry row_field row_frames row_metadata \
                        row_pixels; do
                        [ "$scope" = round ] || continue
                        evidence_stage=$((evidence_stage + 1))
                        row_last=$((row_frames - 1))
                        case $row_codec in
                            h264|mpeg2)
                                row_progress="probe=gstreamer-playback pass=0 frame-index=$row_last pts=0"
                                ;;
                            *)
                                row_progress="probe=gstreamer-codec-playback frame-index=$row_last pts=0"
                                ;;
                        esac
                        row_log=$(mock_oracle_log "$row_codec" "$row_profile" \
                            "$row_frames" "$row_metadata" "$row_pixels" \
                            round-robin) || return 1
                        mock_evidence_stage "$(printf '%04d-round-%03d-%s' \
                            "$evidence_stage" "$evidence_cycle" "$row_id")" \
                            "$row_progress" 1 "$row_log"
                    done < "$manifest"
                    evidence_cycle=$((evidence_cycle + 1))
                done
                while IFS="$tab" read -r scope row_id rest; do
                    [ "$scope" = round ] || continue
                    printf '%s\tsamples=100\tRSS-early-avg=1000\tRSS-late-avg=1000\tRSS-max=1000\tFD-early-avg=4.0\tFD-late-avg=4.0\tthreads-early-avg=2.0\tthreads-late-avg=2.0\ttrend=checked\n' \
                        "$row_id" >> "$evidence_dir/resource-summary.tsv"
                done < "$manifest"
                ;;
            controls)
                while [ "$evidence_stage" -lt 10 ]; do
                    evidence_stage=$((evidence_stage + 1))
                    mock_evidence_stage "$(printf '%04d-controls-%03d' \
                        "$evidence_stage" "$evidence_stage")" \
                        'probe=gstreamer-controls phase=seek-0s-restored-1x frame-identity=359 barcode=359 pts=0 total-video=360'
                done
                evidence_stage=$((evidence_stage + 1))
                mock_evidence_stage "0011-controls-immediate-reopen" \
                    'probe=gstreamer-playback pass=0 frame-index=359 pts=0'
                ;;
            reload-sanity)
                mock_evidence_stage 0001-reload-before \
                    'probe=library-drain iteration=1 frame-index=359 token=100000'
                mock_evidence_stage 0002-reload-after \
                    'probe=library-drain iteration=1 frame-index=359 token=100000'
                ;;
            soak)
                soak_last=$((fixture_frames - 1))
                mock_evidence_stage 0001-soak \
                    "probe=gstreamer-controls phase=initial-1x frame-identity=$soak_last barcode=359 pts=0 total-video=$fixture_frames" \
                    $((RESOURCE_STARTUP_SAMPLES + RESOURCE_TREND_SAMPLES))
                ;;
            churn)
                mock_evidence_stage 0001-churn \
                    'probe=library-drain iteration=1000 frame-index=359 token=100000' \
                    $((RESOURCE_STARTUP_SAMPLES + RESOURCE_TREND_SAMPLES))
                ;;
        esac
    done
    saved_repo_dir=$repo_dir
    repo_dir=$evidence_repo
    check_evidence_set "$evidence_root" >/dev/null
    tests=$((tests + 1))

    oracle_log=$evidence_root/oracle/stages/0001-oracle-h264-short.log
    cp "$oracle_log" "$tmp/oracle-log.backup"
    sed "s/$metadata_h264/$metadata_mpeg2/g" "$tmp/oracle-log.backup" > \
        "$oracle_log"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted a nonmatching oracle metadata hash"
        return 1
    fi
    cp "$tmp/oracle-log.backup" "$oracle_log"
    tests=$((tests + 1))

    sed "s/$output_h264/$output_mpeg2/g" "$tmp/oracle-log.backup" > \
        "$oracle_log"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted a nonmatching oracle pixel hash"
        return 1
    fi
    cp "$tmp/oracle-log.backup" "$oracle_log"
    tests=$((tests + 1))

    round_log=$evidence_root/round-robin/stages/0004-round-001-wmv3-short.log
    cp "$round_log" "$tmp/round-log.backup"
    cp "$evidence_root/round-robin/stages/0003-round-001-vc1-short.log" \
        "$round_log"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted a replacement round-robin stage log"
        return 1
    fi
    cp "$tmp/round-log.backup" "$round_log"
    tests=$((tests + 1))

    printf '%s\n' changed >> "$evidence_root/oracle/runtime-fingerprint.after.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted a runtime change within an evidence gate"
        return 1
    fi
    tests=$((tests + 1))
    cp "$evidence_root/oracle/runtime-fingerprint.before.tsv" \
        "$evidence_root/oracle/runtime-fingerprint.after.tsv"

    empty_progress=$evidence_root/soak-720p/stages/0001-soak.progress
    cp "$empty_progress" "$tmp/progress.backup"
    : > "$empty_progress"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted missing stage progress"
        return 1
    fi
    cp "$tmp/progress.backup" "$empty_progress"
    tests=$((tests + 1))

    corrupt_progress=$evidence_root/soak-720p/stages/0001-soak.progress
    cp "$corrupt_progress" "$tmp/corrupt-progress.backup"
    printf '%s\n' 'probe=gstreamer-controls phase=initial-1x frame-identity=1' >> \
        "$corrupt_progress"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted unparseable final probe progress"
        return 1
    fi
    cp "$tmp/corrupt-progress.backup" "$corrupt_progress"
    tests=$((tests + 1))

    cp "$evidence_root/soak-720p/fixture.txt" "$tmp/fixture-identity.backup"
    sed 's/^geometry=1280x720$/geometry=1920x1080/' \
        "$tmp/fixture-identity.backup" > "$evidence_root/soak-720p/fixture.txt"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted fixture identity inconsistent with acceptance"
        return 1
    fi
    cp "$tmp/fixture-identity.backup" "$evidence_root/soak-720p/fixture.txt"
    tests=$((tests + 1))

    cp "$evidence_root/churn/kernel-check.log" "$tmp/kernel-check.backup"
    : > "$evidence_root/churn/kernel-check.log"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted empty kernel validation"
        return 1
    fi
    cp "$tmp/kernel-check.backup" "$evidence_root/churn/kernel-check.log"
    tests=$((tests + 1))

    cp "$evidence_root/soak-1080p/stages.tsv" "$tmp/stages.backup"
    sed 's/^0001-soak\t/0001-wrong\t/' "$tmp/stages.backup" > \
        "$evidence_root/soak-1080p/stages.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted a wrong stage name"
        return 1
    fi
    cp "$tmp/stages.backup" "$evidence_root/soak-1080p/stages.tsv"
    tests=$((tests + 1))

    cp "$evidence_root/oracle/oracle.tsv" "$tmp/oracle.backup"
    printf '%s\n' '# evidence corruption' >> "$evidence_root/oracle/oracle.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted an oracle not committed in the candidate revision"
        return 1
    fi
    cp "$tmp/oracle.backup" "$evidence_root/oracle/oracle.tsv"
    tests=$((tests + 1))

    cp "$evidence_root/oracle/runtime-fingerprint.before.tsv" "$tmp/fingerprint.backup"
    printf '%s\n' fixed-runtime-fingerprint > \
        "$evidence_root/oracle/runtime-fingerprint.before.tsv"
    cp "$evidence_root/oracle/runtime-fingerprint.before.tsv" \
        "$evidence_root/oracle/runtime-fingerprint.after.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted an incomplete runtime fingerprint"
        return 1
    fi
    cp "$tmp/fingerprint.backup" "$evidence_root/oracle/runtime-fingerprint.before.tsv"
    cp "$tmp/fingerprint.backup" "$evidence_root/oracle/runtime-fingerprint.after.tsv"
    tests=$((tests + 1))

    cp "$evidence_root/oracle/identity.txt" "$tmp/identity.backup"
    sed 's/^pci_device=0x1615$/pci_device=0x1612/' "$tmp/identity.backup" > \
        "$evidence_root/oracle/identity.txt"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted non-BCM70015 evidence"
        return 1
    fi
    cp "$tmp/identity.backup" "$evidence_root/oracle/identity.txt"
    tests=$((tests + 1))

    cp "$evidence_root/oracle/resources.tsv" "$tmp/resources.backup"
    printf 'stage\tsample\tepoch\trss-kb\tfds\tthreads\tprocesses\n' > \
        "$evidence_root/oracle/resources.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted missing resource samples"
        return 1
    fi
    cp "$tmp/resources.backup" "$evidence_root/oracle/resources.tsv"
    tests=$((tests + 1))

    cp "$evidence_root/oracle/resource-summary.tsv" \
        "$tmp/resource-summary.backup"
    awk -F '\t' 'BEGIN { OFS="\t" }
        NR == 2 { $6=0 }
        { print }
    ' "$tmp/resources.backup" > "$evidence_root/oracle/resources.tsv"
    awk -F '\t' 'BEGIN { OFS="\t" }
        NR == 2 {
          $8="threads-early-avg=0.0"
          $9="threads-late-avg=0.0"
        }
        { print }
    ' "$tmp/resource-summary.backup" > \
        "$evidence_root/oracle/resource-summary.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted fewer threads than recorded processes"
        return 1
    fi
    cp "$tmp/resources.backup" "$evidence_root/oracle/resources.tsv"
    cp "$tmp/resource-summary.backup" \
        "$evidence_root/oracle/resource-summary.tsv"
    tests=$((tests + 1))

    cp "$evidence_root/soak-720p/resources.tsv" "$tmp/soak-resources.backup"
    cp "$evidence_root/soak-720p/resource-summary.tsv" "$tmp/soak-summary.backup"
    {
        sed -n '1p' "$tmp/soak-resources.backup"
        sed -n '2p' "$tmp/soak-resources.backup"
    } > "$evidence_root/soak-720p/resources.tsv"
    {
        sed -n '1p' "$tmp/soak-summary.backup"
        printf '0001-soak\tsamples=1\tRSS-early-avg=1000\tRSS-late-avg=1000\tRSS-max=1000\tFD-early-avg=4.0\tFD-late-avg=4.0\tthreads-early-avg=2.0\tthreads-late-avg=2.0\ttrend=insufficient-short-stage\n'
    } > "$evidence_root/soak-720p/resource-summary.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted insufficient resource samples for an RC soak"
        return 1
    fi
    cp "$tmp/soak-resources.backup" "$evidence_root/soak-720p/resources.tsv"
    cp "$tmp/soak-summary.backup" "$evidence_root/soak-720p/resource-summary.tsv"
    tests=$((tests + 1))

    awk -F '\t' 'BEGIN { OFS="\t" }
        NR == 1 { print; next }
        { if ($2 >= 4) $4=50000; print }
    ' "$tmp/soak-resources.backup" > "$evidence_root/soak-720p/resources.tsv"
    if check_evidence_set "$evidence_root" >/dev/null 2>&1; then
        error "self-test accepted growing resources with a forged stable summary"
        return 1
    fi
    cp "$tmp/soak-resources.backup" "$evidence_root/soak-720p/resources.tsv"
    tests=$((tests + 1))
    repo_dir=$saved_repo_dir

    trap - EXIT HUP INT TERM
    rm -rf -- "$tmp"
    say "Phase 1 release-gate hardware-free self-test passed: $tests checks"
}

if [ "$#" -eq 1 ] && [ "$1" = --self-test ]; then
    self_test
    exit
fi
if [ "$#" -eq 1 ] && [ "$1" = manifest-template ]; then
    manifest_template
    exit
fi
if [ "$#" -eq 2 ] && [ "$1" = manifest-check ]; then
    validate_manifest "$2"
    exit
fi
if [ "$#" -eq 2 ] && [ "$1" = evidence-check ]; then
    freeze_release_environment
    check_evidence_set "$2"
    exit
fi
[ "$#" -gt 0 ] || usage
mode=$1
shift
case $mode in soak|churn|oracle|round-robin|controls|reload-sanity) ;; *) usage ;; esac

fixture=
input_sha=
codec=
profile=
geometry=
field=
frames=
seconds=
timeout_seconds=
iterations=
cycles=
manifest=
fixture_root=
audio=yes
audio_explicit=no
skip_2x=no

while [ "$#" -gt 0 ]; do
    option=$1
    shift
    case $option in
        --runtime|--results|--fixture|--fixture-root|--input-sha256|--codec|--profile|--geometry|--field|--frames|--seconds|--timeout|--iterations|--cycles|--manifest)
            [ "$#" -gt 0 ] || usage
            value=$1
            shift
            case $option in
                --runtime) runtime=$value ;;
                --results) results=$value ;;
                --fixture) fixture=$value ;;
                --fixture-root) fixture_root=$value ;;
                --input-sha256) input_sha=$value ;;
                --codec) codec=$value ;;
                --profile) profile=$value ;;
                --geometry) geometry=$value ;;
                --field) field=$value ;;
                --frames) frames=$value ;;
                --seconds) seconds=$value ;;
                --timeout) timeout_seconds=$value ;;
                --iterations) iterations=$value ;;
                --cycles) cycles=$value ;;
                --manifest) manifest=$value ;;
            esac
            ;;
        --video-only) audio=no; audio_explicit=yes ;;
        --audio) audio=yes; audio_explicit=yes ;;
        --skip-2x) skip_2x=yes ;;
        --rc) [ "$rc" = no ] || usage; rc=yes ;;
        *) usage ;;
    esac
done

case $mode in
    round-robin)
        [ "$audio_explicit" = no ] && [ "$skip_2x" = no ] && \
            [ -z "$iterations" ] || usage
        ;;
    oracle)
        [ "$audio_explicit" = no ] && [ "$skip_2x" = no ] && \
            [ -z "$iterations$cycles" ] || usage
        ;;
    soak)
        [ -z "$manifest$fixture_root$iterations$cycles" ] && [ "$skip_2x" = no ] || usage
        ;;
    controls)
        [ -z "$manifest$fixture_root$seconds$iterations" ] || usage
        ;;
    churn|reload-sanity)
        [ -z "$manifest$fixture_root$seconds$cycles" ] && [ "$audio_explicit" = no ] && [ "$skip_2x" = no ] || usage
        [ "$mode" = churn ] || [ -z "$iterations" ] || usage
        ;;
esac

[ "$rc" != yes ] && [ "$mode" != oracle ] || freeze_release_environment
set_acceptance_label
prepare_run
if [ "$mode" != oracle ] && [ "$mode" != round-robin ]; then
    validate_common_fixture
fi
case $mode in
    soak) run_soak ;;
    churn) run_churn ;;
    oracle) run_oracle ;;
    round-robin) run_round_robin ;;
    controls) run_controls ;;
    reload-sanity) run_reload_sanity ;;
esac
finish_run
