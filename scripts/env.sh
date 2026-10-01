# Source from Git Bash: . scripts/env.sh
export VITASDK="C:/vitasdk-softfp"
export PATH="/c/vitasdk-softfp/bin:$PATH"
MINGW_BIN="$(dirname "$(command -v mingw32-make)")"
# Makefiles expect a plain `make`
mkdir -p "$HOME/.local/vitabin"
[ -e "$HOME/.local/vitabin/make.exe" ] || cp "$MINGW_BIN/mingw32-make.exe" "$HOME/.local/vitabin/make.exe"
export PATH="$HOME/.local/vitabin:$PATH"
export CMAKE_GENERATOR="Unix Makefiles"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
