#!/bin/bash

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "$0")"; pwd)"
SERVER="$SCRIPT_DIR/cxadc_vhs_server"
TEMP_DIR="$(mktemp --directory)"
SOCKET="$TEMP_DIR/server.sock"

#CXADC_VHS_SERVER_WORKS="$("$SERVER" version >/dev/null && echo "0" || echo "$?")"
#JQ_WORKS="$( (echo '0' | jq .) || echo "$?")"

# test the commands we need. if they fail, user will be appropriately notified by bash

"$SERVER" version >/dev/null
echo '0' | jq . >/dev/null
curl --version >/dev/null

#RESAMPLE_HIFI=0

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
    --help)
      SHOW_HELP="YES"
      shift # past argument with no value
      ;;
    --debug)
      set -x
      shift # past argument with no value
      ;;
#    --no-resample)
#      RESAMPLE_HIFI=1
#      shift # past argument with no value
#      ;;
    -*)
      echo "Unknown option $i"
      exit 1
      ;;
    *)
      ;;
  esac
done

function usage
{
	echo "Usage: $SCRIPT_NAME [options] <basepath>" >&2
	printf "\t--video=#         Number of CX card to use for video capture\n" >&2
	printf "\t--hifi=#          Number of CX card to use for hifi capture\n" >&2
	printf "\t--linear=<device> ALSA device identifier for linear\n" >&2
	printf "\t--debug           Show commands executed\n" >&2
	printf "\t--help            Show usage information\n" >&2
	exit 1
}

if [[ -n "${SHOW_HELP-}" ]]; then
    usage;
fi

if [[ -z "${1-}" ]]; then
    echo "Error: No path provided." >&2
    usage;
fi

OUTPUT_BASEPATH="$1"

function die
{
	echo "$@" >&2
    if [[ -n "${SERVER_PID-}" ]]; then
        kill "$SERVER_PID" || true;
    fi
    if [[ -n "${VIDEO_PID-}" ]]; then
        kill "$VIDEO_PID" || true;
    fi
    if [[ -n "${HIFI_PID-}" ]]; then
        kill "$HIFI_PID" || true;
    fi
    if [[ -n "${LINEAR_PID-}" ]]; then
        kill "$LINEAR_PID" || true;
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
    ((CXADC_COUNTER+=1))
    START_URL="${START_URL}cxadc$CXADC_VIDEO&";
fi
if [[ -n "${CXADC_HIFI-}" ]]; then
    HIFI_IDX="$CXADC_COUNTER"
    ((CXADC_COUNTER+=1))
    START_URL="${START_URL}cxadc$CXADC_HIFI&";
fi

if [[ -n "${LINEAR_DEVICE-}" ]]; then
    START_URL="${START_URL}lname=$LINEAR_DEVICE&";
fi

START_RESULT=$(curl -X GET --unix-socket "$SOCKET" -s "$START_URL" || die "Cannot send start request to server: $?");
echo "$START_RESULT" | jq -r .state | xargs test "Running" "=" || die "Server failed to start capture: $(echo "$START_RESULT" | jq -r .fail_reason)"


if [[ -n "${VIDEO_IDX-}" ]]; then
    VIDEO_PATH="$OUTPUT_BASEPATH-video.u8"
    curl -X GET --unix-socket "$SOCKET" -s --output "$VIDEO_PATH" "http:/d/cxadc?$VIDEO_IDX" &
    VIDEO_PID=$!
    echo "PID $VIDEO_PID is capturing video to $VIDEO_PATH"
fi
if [[ -n "${HIFI_IDX-}" ]]; then
    HIFI_PATH="$OUTPUT_BASEPATH-hifi.u8"
    curl -X GET --unix-socket "$SOCKET" -s --output "$HIFI_PATH" "http:/d/cxadc?$HIFI_IDX" &
    HIFI_PID=$!
    echo "PID $HIFI_PID is capturing hifi to $HIFI_PATH"
fi

LINEAR_PATH="$OUTPUT_BASEPATH-linear.s24"
curl -X GET --unix-socket "$SOCKET" -s --output "$LINEAR_PATH" "http:/d/linear" &
LINEAR_PID=$!
echo "PID $LINEAR_PID is capturing linear to $LINEAR_PATH"

echo "Capture running..."
echo "Press Ctrl+C to stop recording"
( trap exit SIGINT ; read -r -d '' _ </dev/tty )
echo ""
echo "Stopping capture"

STOP_RESULT=$(curl -X GET --unix-socket "$SOCKET" -s "http:/d/stop" || die "Cannot send stop request to server: $?");
echo "$STOP_RESULT" | jq -r .state | xargs test "Idle" "=" || die "Server failed to stop capture: ${STOP_RESULT}"
echo "$STOP_RESULT" | jq -r .overflows | xargs printf "Encountered %d overflows during capture\n" || die "Can't find overflow information"

echo "Waiting for writes to finish..."

for PID in ${VIDEO_PID-} ${HIFI_PID-} $LINEAR_PID ; do
	wait "$PID" || true
done

echo "Killing server"

kill $SERVER_PID

echo "Finished!"
