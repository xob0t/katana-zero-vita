# Source from Git Bash: . scripts/env.sh
# On Windows this points at the softfp VitaSDK from setup-sdk.sh and provides a plain `make`. Elsewhere (the CI
# container, which sets VITASDK itself) it leaves the environment as it is.
if [ -z "${VITASDK:-}" ] || [ -d /c/vitasdk-softfp ]; then
	export VITASDK="C:/vitasdk-softfp"
	export PATH="/c/vitasdk-softfp/bin:$PATH"
fi
if command -v mingw32-make >/dev/null 2>&1; then
	MINGW_BIN="$(dirname "$(command -v mingw32-make)")"
	# Makefiles expect a plain `make`
	mkdir -p "$HOME/.local/vitabin"
	[ -e "$HOME/.local/vitabin/make.exe" ] || cp "$MINGW_BIN/mingw32-make.exe" "$HOME/.local/vitabin/make.exe"
	export PATH="$HOME/.local/vitabin:$PATH"
	export CMAKE_GENERATOR="Unix Makefiles"
fi
export CMAKE_POLICY_VERSION_MINIMUM=3.5
