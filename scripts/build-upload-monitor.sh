#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

ENV_NAME="tdeck"
TDECK_ENV_NAME="tdeck"
TPAGER_ENV_NAME="tpager"
CARDPUTER_ENV_NAME="cardputer"
M9_ENV_NAME="m9"
WIO_TRACKER_L2_ENV_NAME="wio_tracker_l2"
TDECK_COOP_ENV_NAME="tdeck_cooperative"
TPAGER_COOP_ENV_NAME="tpager_cooperative"
ENV_EXPLICIT=false
ERASE_FIRST=false
FULLCLEAN=false
JUST_BUILD=false
PORT=""

has_env() {
	local env_name="$1"
	grep -q "^\[env:${env_name}\]" platformio.ini
}

# Every [env:NAME] in platformio.ini, in file order. Read from the file rather
# than from the list of device flags below so a newly added environment is
# covered by --just-build the day it lands, without touching this script.
all_envs() {
	sed -n 's/^\[env:\(.*\)\]$/\1/p' platformio.ini
}

# Human label for an env, for the menu and the --just-build summary. Falls back
# to the env name itself, which is what any environment not listed here gets.
env_label() {
	case "$1" in
		"$TDECK_ENV_NAME")          echo "LilyGo T-Deck Plus" ;;
		"$TPAGER_ENV_NAME")         echo "LilyGo T-Lora Pager" ;;
		"$CARDPUTER_ENV_NAME")      echo "M5Stack Cardputer Adv" ;;
		"$M9_ENV_NAME")             echo "Elecrow ThinkNode M9" ;;
		"$WIO_TRACKER_L2_ENV_NAME") echo "Seeed Wio Tracker L2" ;;
		"$TDECK_COOP_ENV_NAME")     echo "LilyGo T-Deck Plus (cooperative scheduler, debug)" ;;
		"$TPAGER_COOP_ENV_NAME")    echo "LilyGo T-Lora Pager (cooperative scheduler, debug)" ;;
		*)                          echo "$1" ;;
	esac
}

select_env_or_exit() {
	local env_name="$1"

	if has_env "$env_name"; then
		ENV_NAME="$env_name"
		ENV_EXPLICIT=true
		return
	fi

	echo "Environment '$env_name' not found in platformio.ini"
	exit 1
}

prompt_for_device() {
	local options=()
	local env

	while IFS= read -r env; do
		[ -n "$env" ] && options+=("$env")
	done < <(all_envs)

	if [ "${#options[@]}" -eq 0 ]; then
		echo "No device environments found in platformio.ini"
		exit 1
	fi

	if [ ! -t 0 ]; then
		ENV_NAME="${options[0]}"
		echo "[PIO] Non-interactive shell detected, using device env: $ENV_NAME"
		return
	fi

	echo "Select device to build for:"
	for i in "${!options[@]}"; do
		printf "  %d) %s (%s)\n" "$((i + 1))" "$(env_label "${options[$i]}")" "${options[$i]}"
	done

	while true; do
		read -r -p "Enter choice [1-${#options[@]}]: " choice
		if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "${#options[@]}" ]; then
			ENV_NAME="${options[$((choice - 1))]}"
			echo "[PIO] Selected device env: $ENV_NAME"
			return
		fi
		echo "Invalid selection. Please choose a number between 1 and ${#options[@]}."
	done
}

show_usage() {
	echo "Usage: $0 [--tdeck|-t] [--pager|-P] [--cardputer|-C] [--m9|-9] [--wio-tracker-l2|-W]"
	echo "          [--tdeck-cooperative] [--tpager-cooperative]"
	echo "          [--erase|-E] [--fullclean|-F] [--just-build|-B] [--port PORT]"
	echo "  --tdeck, -t          Use T-Deck Plus environment ($TDECK_ENV_NAME)"
	echo "  --pager, -P          Use T-Lora Pager environment ($TPAGER_ENV_NAME)"
	echo "  --cardputer, -C      Use Cardputer Adv environment ($CARDPUTER_ENV_NAME)"
	echo "  --m9, -9             Use Elecrow ThinkNode M9 environment ($M9_ENV_NAME)"
	echo "  --wio-tracker-l2, -W Use Seeed Wio Tracker L2 environment ($WIO_TRACKER_L2_ENV_NAME)"
	echo "  --tdeck-cooperative  Use T-Deck cooperative-scheduler debug env ($TDECK_COOP_ENV_NAME)"
	echo "  --tpager-cooperative Use T-Pager cooperative-scheduler debug env ($TPAGER_COOP_ENV_NAME)"
	echo "                       If no device flag is given, you'll be prompted to choose."
	echo "  --erase, -E          Erase the whole flash before upload (wipes settings/storage)"
	echo "  --fullclean, -F      Run PlatformIO fullclean before building"
	echo "  --just-build, -B     Compile only - no upload, no monitor, no device needed."
	echo "                       Builds every environment in platformio.ini, or just the"
	echo "                       one named by a device flag. Keeps going after a failure"
	echo "                       and prints a pass/fail summary; exits non-zero if any"
	echo "                       environment failed to build."
	echo "  --port PORT          Serial port for erase/upload/monitor (default: auto-detect)"
}

run_pio_target() {
	local target="$1"
	local label="$2"
	local port_args=()

	if [ -n "$PORT" ]; then
		case "$target" in
			monitor) port_args=(--monitor-port "$PORT") ;;
			fullclean) ;;
			*) port_args=(--upload-port "$PORT") ;;
		esac
	fi

	echo "[PIO] $label ($ENV_NAME)..."
	pio run -e "$ENV_NAME" -t "$target" ${port_args[@]+"${port_args[@]}"}
}

format_duration() {
	local total_seconds="$1"
	local hours=$((total_seconds / 3600))
	local minutes=$(((total_seconds % 3600) / 60))
	local seconds=$((total_seconds % 60))

	if [ "$hours" -gt 0 ]; then
		printf "%dh %02dm %02ds" "$hours" "$minutes" "$seconds"
	else
		printf "%dm %02ds" "$minutes" "$seconds"
	fi
}

if ! command -v pio >/dev/null 2>&1; then
	echo "PlatformIO CLI not found: install it or run from an environment that provides 'pio'."
	exit 1
fi

while [ "$#" -gt 0 ]; do
	case "$1" in
		--tdeck|-t)
			select_env_or_exit "$TDECK_ENV_NAME"
			;;
		--pager|--tpager|-P)
			select_env_or_exit "$TPAGER_ENV_NAME"
			;;
		--cardputer|-C)
			select_env_or_exit "$CARDPUTER_ENV_NAME"
			;;
		--m9|-9)
			select_env_or_exit "$M9_ENV_NAME"
			;;
		--wio-tracker-l2|--wio|-W)
			select_env_or_exit "$WIO_TRACKER_L2_ENV_NAME"
			;;
		--tdeck-cooperative)
			select_env_or_exit "$TDECK_COOP_ENV_NAME"
			;;
		--tpager-cooperative)
			select_env_or_exit "$TPAGER_COOP_ENV_NAME"
			;;
		--erase|-E)
			ERASE_FIRST=true
			;;
		--fullclean|-F)
			FULLCLEAN=true
			;;
		--just-build|-B)
			JUST_BUILD=true
			;;
		--port)
			if [ "$#" -lt 2 ] || [ -z "$2" ]; then
				echo "--port needs a serial port argument, e.g. --port /dev/cu.usbmodem1101"
				exit 1
			fi
			PORT="$2"
			shift
			;;
		--port=*)
			PORT="${1#--port=}"
			;;
		--help|-h)
			show_usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1"
			show_usage
			exit 1
			;;
	esac
	shift
done

if [ "$JUST_BUILD" = true ]; then
	if [ "$ERASE_FIRST" = true ]; then
		echo "--erase needs a connected device; it cannot be combined with --just-build."
		exit 1
	fi

	# A device flag narrows the sweep to that one environment: "--just-build -t"
	# is the natural way to compile-check the board you are working on without
	# a cable. With no device flag, every environment gets built.
	BUILD_ENVS=()
	if [ "$ENV_EXPLICIT" = true ]; then
		BUILD_ENVS+=("$ENV_NAME")
	else
		while IFS= read -r env; do
			[ -n "$env" ] && BUILD_ENVS+=("$env")
		done < <(all_envs)
	fi

	if [ "${#BUILD_ENVS[@]}" -eq 0 ]; then
		echo "No environments found in platformio.ini"
		exit 1
	fi

	echo "[PIO] Build-only sweep over ${#BUILD_ENVS[@]} environment(s)."
	SWEEP_START_TS="$(date +%s)"
	RESULTS=()
	FAILED=0

	for env in "${BUILD_ENVS[@]}"; do
		echo
		echo "===================================================================="
		echo "[PIO] Building $env ($(env_label "$env"))"
		echo "===================================================================="

		ENV_START_TS="$(date +%s)"
		if [ "$FULLCLEAN" = true ]; then
			echo "[PIO] Full clean ($env)..."
			# A clean failure is the env's failure: building on top of a
			# half-cleaned tree would report a result about the wrong sources.
			if ! pio run -e "$env" -t fullclean; then
				ENV_END_TS="$(date +%s)"
				RESULTS+=("FAIL  $env  (fullclean)  $(format_duration $((ENV_END_TS - ENV_START_TS)))")
				FAILED=$((FAILED + 1))
				continue
			fi
		fi

		# No -t: the default target is a plain build. Failures are collected
		# rather than aborting the sweep — the point of building everything is
		# to find out which ones are broken, not just the first.
		if pio run -e "$env"; then
			ENV_END_TS="$(date +%s)"
			SIZE_NOTE=""
			ENV_BIN=".pio/build/${env}/firmware.bin"
			if [ -f "$ENV_BIN" ]; then
				SIZE_NOTE="  $(( $(wc -c <"$ENV_BIN") / 1024 )) KB"
			fi
			RESULTS+=("ok    $env  $(format_duration $((ENV_END_TS - ENV_START_TS)))${SIZE_NOTE}")
		else
			ENV_END_TS="$(date +%s)"
			RESULTS+=("FAIL  $env  $(format_duration $((ENV_END_TS - ENV_START_TS)))")
			FAILED=$((FAILED + 1))
		fi
	done

	SWEEP_END_TS="$(date +%s)"
	echo
	echo "===================================================================="
	echo "[PIO] Build summary"
	echo "===================================================================="
	for line in "${RESULTS[@]}"; do
		echo "  $line"
	done
	echo
	echo "[PIO] ${#BUILD_ENVS[@]} environment(s), $FAILED failed, total $(format_duration $((SWEEP_END_TS - SWEEP_START_TS)))."

	if [ "$FAILED" -gt 0 ]; then
		exit 1
	fi
	exit 0
fi

if [ "$ENV_EXPLICIT" = false ]; then
	prompt_for_device
fi

if [ "$ERASE_FIRST" = true ]; then
	run_pio_target "erase" "Erasing device flash"
fi

BUILD_START_TS="$(date +%s)"
if [ "$FULLCLEAN" = true ]; then
	run_pio_target "fullclean" "Full clean"
fi
run_pio_target "upload" "Upload"
BUILD_END_TS="$(date +%s)"
BUILD_ELAPSED_SECS=$((BUILD_END_TS - BUILD_START_TS))
echo "[PIO] Build completed in $(format_duration "$BUILD_ELAPSED_SECS")."

ELF_PATH=".pio/build/${ENV_NAME}/firmware.elf"
BIN_PATH=".pio/build/${ENV_NAME}/firmware.bin"
if [ -f "$ELF_PATH" ]; then
	ELF_SHA="$(shasum -a 256 "$ELF_PATH" | awk '{print $1}')"
	echo "[PIO] ELF SHA256: $ELF_SHA"
	echo "[PIO] Runtime monitor should show: ELF file SHA256: ${ELF_SHA:0:16}"
fi
if [ -f "$BIN_PATH" ]; then
	BIN_SHA="$(shasum -a 256 "$BIN_PATH" | awk '{print $1}')"
	echo "[PIO] BIN SHA256: $BIN_SHA"
fi

run_pio_target "monitor" "Monitor"
