# Copy to build-env.sh (gitignored) and `source` it before building.
#
# These override whatever is in sdkconfig, which is why no real address needs
# to be committed. CMake reads them at configure time, so after changing one:
#   idf.py reconfigure && idf.py build
export BEEBO_PROXY_URL="ws://192.168.1.100:18797/ws"

# Optional. Usually better left unset - the board has a wifi picker in
# settings and what you type there is stored in NVS rather than in the binary.
# export BEEBO_WIFI_SSID="..."
# export BEEBO_WIFI_PASS="..."

# Must match RELAY_TOKEN in services/beebo-proxy/.env, or be empty on both.
# export BEEBO_PROXY_TOKEN=""

# POSIX TZ. Note the inverted sign: UTC+8 is written "-8".
# export BEEBO_TZ="CST-8"

# Identifies this board to the proxy; change it if you run more than one.
# export BEEBO_DEVICE_ID="beebo"
