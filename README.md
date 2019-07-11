## Skia
This fork maintains an earlier version of Skia from Mar 3 2017 with a few minor fixes added.

Skia is a complete 2D graphic library for drawing Text, Geometries, and Images.

See full details at https://skia.org.

## Build
Install Chromium's depot tools. Follow the instructions at http://www.chromium.org/developers/how-tos/install-depot-tools.

The build steps below should be run from an x64 Native Command Prompt for Visual Studio.

```bash
# Clone the source
git clone https://github.com/JaFenix/skia.git

# Change to the source directory
cd skia

# Download/install project dependencies
python tools\git-sync-deps

# Generate the platform build environment
gn gen out\win_x64\release

# Build skia.dll
ninja -C out\win_x64\release skia
