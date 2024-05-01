#!/bin/bash

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(
	cd "$(dirname "$0")"
	pwd
)"
SERVER="$SCRIPT_DIR/cxadc_vhs_server"
TEMP_DIR="$(mktemp --directory)"
SOCKET="$TEMP_DIR/server.sock"

# test the commands we need. if they fail, user will be appropriately notified by bash

"$SERVER" version >/dev/null
jq --version >/dev/null
curl --version >/dev/null

if "$SCRIPT_DIR/ffmpeg" -version &>/dev/null; then
	FFMPEG_CMD="$SCRIPT_DIR/ffmpeg"
fi

if "ffmpeg" -version &>/dev/null; then
	FFMPEG_CMD="ffmpeg"
fi

if [[ -z "${FFMPEG_CMD-}" ]]; then
	echo "No working ffmpeg found, some features may be unavailable. Obtain a binary here: https://johnvansickle.com/ffmpeg/"
else
	if "$FFMPEG_CMD" -version | grep -- '--enable-libsoxr' &>/dev/null; then
		FFMPEG_HAS_LIBSOXR=1
	else
		echo "Your ffmpeg is not built with libsoxr, some features may be unavailable"
	fi
fi

for i in "$@"; do
	case $i in
	--video=*)
		CXADC_VIDEO="${i#*=}"
		shift # past argument=value
		;;
	--hifi=*)
		CXADC_HIFI="${i#*=}"
		shift # past argument=value
		;;
	--linear=*)
		LINEAR_DEVICE="${i#*=}"
		shift # past argument=value
		;;
	--add-date)
		ADD_DATE="YES"
		shift # past argument with no value
		;;
	--convert-linear)
		CONVERT_LINEAR="YES"
		shift # past argument with no value
		;;
	--compress-video)
		COMPRESS_VIDEO="YES"
		shift # past argument with no value
		;;
	--compress-hifi)
		COMPRESS_HIFI="YES"
		shift # past argument with no value
		;;
	--resample-hifi)
		RESAMPLE_HIFI="YES"
		shift # past argument with no value
		;;
	--help)
		SHOW_HELP="YES"
		shift # past argument with no value
		;;
	--debug)
		set -x
		shift # past argument with no value
		;;
	-*)
		echo "Unknown option $i"
		exit 1
		;;
	*) ;;

	esac
done

function usage {
	echo "Usage: $SCRIPT_NAME [options] <basepath>" >&2
	printf "\t--video=          Number of CX card to use for video capture (unset=disabled)\n" >&2
	printf "\t--hifi=           Number of CX card to use for hifi capture (unset=disabled)\n" >&2
	printf "\t--linear=         ALSA device identifier for linear (unset=default)\n" >&2
	printf "\t--add-date        Add current date and time to the filenames\n" >&2
	printf "\t--convert-linear  Convert linear to flac+u8\n" >&2
	printf "\t--compress-video  Compress video\n" >&2
	printf "\t--compress-hifi   Compress hifi\n" >&2
	printf "\t--resample-hifi   Resample hifi to 10 MSps\n" >&2
	printf "\t--debug           Show commands executed\n" >&2
	printf "\t--help            Show usage information\n" >&2
	exit 1
}

if [[ -n "${SHOW_HELP-}" ]]; then
	usage
fi

if [[ -z "${1-}" ]]; then
	echo "Error: No path provided." >&2
	usage
fi

if [ -z "${FFMPEG_CMD-}" ]; then
	[ -n "${CONVERT_LINEAR-}" ] || echo "Converting linear requires ffmpeg." && exit 1
	[ -n "${COMPRESS_VIDEO-}" ] || echo "Compressing video requires ffmpeg." && exit 1
	[ -n "${COMPRESS_HIFI-}" ] || echo "Compressing hifi requires ffmpeg." && exit 1
	[ -n "${RESAMPLE_HIFI-}" ] || echo "Resampling hifi requires ffmpeg." && exit 1
fi

if [ -n "${RESAMPLE_HIFI-}" ] && [ -z "${FFMPEG_HAS_LIBSOXR-}" ]; then
	echo "Resampling hifi requires an ffmpeg build with libsoxr."
	exit 1
fi

OUTPUT_BASEPATH="$1"

if [[ -n "${ADD_DATE-}" ]]; then
	OUTPUT_BASEPATH="$OUTPUT_BASEPATH-$(date -Iseconds | sed 's/[T:\+]/_/g')"
fi

function die {
	echo "$@" >&2
	if [[ -n "${SERVER_PID-}" ]]; then
		kill "$SERVER_PID" || true
	fi
	if [[ -n "${VIDEO_PID-}" ]]; then
		kill "$VIDEO_PID" || true
	fi
	if [[ -n "${HIFI_PID-}" ]]; then
		kill "$HIFI_PID" || true
	fi
	if [[ -n "${LINEAR_PID-}" ]]; then
		kill "$LINEAR_PID" || true
	fi
	exit 1
}

"$SERVER" "unix:$SOCKET" &
SERVER_PID=$!

echo "Server started (PID $SERVER_PID)"

# wait for the server to listen
sleep 0.5

# test that the server runs
curl -X GET --unix-socket "$SOCKET" -s "http:/d/" >/dev/null || die "Server unreachable: $?"

CXADC_COUNTER=0

START_URL="http:/d/start?"
if [[ -n "${CXADC_VIDEO-}" ]]; then
	VIDEO_IDX="$CXADC_COUNTER"
	((CXADC_COUNTER += 1))
	START_URL="${START_URL}cxadc$CXADC_VIDEO&"
fi

if [[ -n "${CXADC_HIFI-}" ]]; then
	HIFI_IDX="$CXADC_COUNTER"
	((CXADC_COUNTER += 1))
	START_URL="${START_URL}cxadc$CXADC_HIFI&"
fi

if [[ -n "${LINEAR_DEVICE-}" ]]; then
	START_URL="${START_URL}lname=$LINEAR_DEVICE&"
fi

START_RESULT=$(curl -X GET --unix-socket "$SOCKET" -s "$START_URL" || die "Cannot send start request to server: $?")
echo "$START_RESULT" | jq -r .state | xargs test "Running" "=" || die "Server failed to start capture: $(echo "$START_RESULT" | jq -r .fail_reason)"
LINEAR_RATE="$(echo "$START_RESULT" | jq -r .linear_rate)"

if [[ -n "${VIDEO_IDX-}" ]]; then
	if [[ -z "${COMPRESS_VIDEO-}" ]]; then
		VIDEO_PATH="$OUTPUT_BASEPATH-video.u8"
		curl -X GET --unix-socket "$SOCKET" -s --output "$VIDEO_PATH" "http:/d/cxadc?$VIDEO_IDX" &
	else
		VIDEO_PATH="$OUTPUT_BASEPATH-video.ldf"
		curl -X GET --unix-socket "$SOCKET" -s --output - "http:/d/cxadc?$VIDEO_IDX" | "$FFMPEG_CMD" \
			-hide_banner -loglevel error \
			-ar 40000 -f u8 -i - \
			-compression_level 0 -f flac "$VIDEO_PATH" &
	fi
	VIDEO_PID=$!
	echo "PID $VIDEO_PID is capturing video to $VIDEO_PATH"
fi
if [[ -n "${HIFI_IDX-}" ]]; then
	if [[ -n "${RESAMPLE_HIFI-}" ]]; then
		RESAMPLE_FILTERS="-filter_complex aresample=resampler=soxr:precision=15,aformat=sample_fmts=u8:sample_rates=10000"
	else
		RESAMPLE_FILTERS=""
	fi

	if [[ -z "${COMPRESS_HIFI-}" ]]; then
		HIFI_PATH="$OUTPUT_BASEPATH-hifi.u8"
		if [[ -z "${RESAMPLE_HIFI-}" ]]; then
			curl -X GET --unix-socket "$SOCKET" -s --output "$HIFI_PATH" "http:/d/cxadc?$HIFI_IDX" &
		else
			curl -X GET --unix-socket "$SOCKET" -s --output - "http:/d/cxadc?$HIFI_IDX" | "$FFMPEG_CMD" \
				-hide_banner -loglevel error \
				-ar 40000 -f u8 -i - \
				$RESAMPLE_FILTERS \
				-compression_level 0 -f u8 "$HIFI_PATH" &
		fi
	else
		HIFI_PATH="$OUTPUT_BASEPATH-hifi.flac"
		curl -X GET --unix-socket "$SOCKET" -s --output - "http:/d/cxadc?$HIFI_IDX" | "$FFMPEG_CMD" \
			-hide_banner -loglevel error \
			-ar 40000 -f u8 -i - \
			$RESAMPLE_FILTERS \
			-compression_level 0 -f flac "$HIFI_PATH" &
	fi
	HIFI_PID=$!
	echo "PID $HIFI_PID is capturing hifi to $HIFI_PATH"
fi

if [[ -n "${CONVERT_LINEAR-}" ]]; then
	HEADSWITCH_PATH="$OUTPUT_BASEPATH-headswitch.u8"
	LINEAR_PATH="$OUTPUT_BASEPATH-linear.flac"
	curl -X GET --unix-socket "$SOCKET" -s --output - "http:/d/linear" | "$FFMPEG_CMD" \
		-hide_banner -loglevel error \
		-ar "$LINEAR_RATE" -ac 3 -f s24le -i - \
		-filter_complex "[0:a]channelsplit=channel_layout=2.1[FL][FR][headswitch],[FL][FR]amerge=inputs=2[linear]" \
		-map "[linear]" -compression_level 0 "$LINEAR_PATH" \
		-map "[headswitch]" -f u8 "$HEADSWITCH_PATH" &
	LINEAR_PID=$!
	echo "PID $LINEAR_PID is capturing linear to $LINEAR_PATH, headswitch to $HEADSWITCH_PATH"
else
	LINEAR_PATH="$OUTPUT_BASEPATH-linear.s24"
	curl -X GET --unix-socket "$SOCKET" -s --output "$LINEAR_PATH" "http:/d/linear" &
	LINEAR_PID=$!
	echo "PID $LINEAR_PID is capturing linear+headswitch to $LINEAR_PATH"
fi

SECONDS=0

echo "Capture running... Press 'q' to stop the capture."

while true; do
	ELAPSED=$SECONDS
	STATS=$(curl -X GET --unix-socket "$SOCKET" -s --output - "http:/d/stats" || true);
	if [[ -z "${STATS-}" ]]; then
		STATS_MSG="Failed to get stats."
	else
		STATS_MSG="Buffers: $(echo "$STATS" | jq .linear.difference_pct | xargs printf '% 2s%% ')$(echo "$STATS" | jq .cxadc[].difference_pct | xargs printf '% 2s%% ')"
	fi
	echo "Capturing for $((ELAPSED / 60))m $((ELAPSED % 60))s... $STATS_MSG"
	if read -r -t 5 -n 1 key; then
		if [[ $key == "q" ]]; then
			echo -e "\nStopping capture"
			break
		else
			echo -e "\nPress 'q' to stop the capture."
		fi
	fi
done

STOP_RESULT=$(curl -X GET --unix-socket "$SOCKET" -s "http:/d/stop" || die "Cannot send stop request to server: $?")
echo "$STOP_RESULT" | jq -r .state | xargs test "Idle" "=" || die "Server failed to stop capture: ${STOP_RESULT}"
echo "$STOP_RESULT" | jq -r .overflows | xargs printf "Encountered %d overflows during capture\n" || die "Can't find overflow information"

echo "Waiting for writes to finish..."

for PID in ${VIDEO_PID-} ${HIFI_PID-} $LINEAR_PID; do
	wait "$PID" || true
done

echo "Killing server"

kill $SERVER_PID

echo "Finished!"
