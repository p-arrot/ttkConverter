#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 || ($1 != x86 && $1 != x64 && $1 != modern) ]]; then
    echo 'Usage: build-media.sh x86|x64|modern' >&2
    exit 2
fi

arch=$1
build_root=${TTK_BUILD_ROOT:-}
if [[ -z $build_root ]]; then
    build_root=${LOCALAPPDATA:?Set TTK_BUILD_ROOT to the Windows build workspace}/ttkconverter-build
fi
build_root=$(cygpath -u "$build_root")
src=$build_root/source
work=$build_root/media-$arch
prefix=$work/install
stage=$work/engines
mkdir -p "$prefix" "$stage"

codec_prefix=$prefix
hardware=()
if [[ $arch == modern ]]; then
    codec_prefix=$build_root/media-x64/install
    TTK_MEDIA_SCOPE=ffmpeg
    hardware=(--enable-ffnvcodec --enable-nvenc --enable-amf
        --enable-encoder=h264_nvenc,hevc_nvenc,h264_amf,hevc_amf)
fi

if [[ $arch == x86 ]]; then
    cpu=i686
    ff_arch=x86
    aom_cpu=x86
    vpx_target=x86-win32-gcc
    lame_cflags=-msse2
else
    cpu=x86_64
    ff_arch=x86_64
    aom_cpu=x86_64
    vpx_target=x86_64-win64-gcc
    lame_cflags=
fi

triple=$cpu-w64-mingw32
toolchain=${TTK_LLVM_ROOT:-$build_root/toolchain}/bin
export PATH="$toolchain:$src/nasm/nasm-2.16.03:$PATH"
export CC=$triple-clang
export CXX=$triple-clang++
export AR=$triple-ar
export RANLIB=$triple-ranlib
export STRIP=$triple-strip
export NM=$triple-nm
export PKG_CONFIG=pkgconf
export PKG_CONFIG_LIBDIR=$(cygpath -m "$codec_prefix/lib/pkgconfig")
export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR
jobs=${TTK_BUILD_JOBS:-8}
prefix_win=$(cygpath -m "$prefix")
codec_win=$(cygpath -m "$codec_prefix")
tool_win=$(cygpath -m "$toolchain")
build_flags='-D_WIN32_WINNT=0x0601 -DWINVER=0x0601'

wants() {
    [[ -z ${TTK_MEDIA_SCOPE:-} || " $TTK_MEDIA_SCOPE " == *" $1 "* ]]
}

build_cmake() {
    local name=$1 source=$2 install_dir=$3
    shift 3
    cmake -S "$source" -B "$work/$name" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR="$cpu" \
        -DCMAKE_C_COMPILER="$tool_win/$CC.exe" -DCMAKE_CXX_COMPILER="$tool_win/$CXX.exe" \
        -DCMAKE_RC_COMPILER="$tool_win/$triple-windres.exe" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$install_dir" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_PREFIX_PATH="$prefix_win" \
        -DCMAKE_C_FLAGS="$build_flags" -DCMAKE_CXX_FLAGS="$build_flags" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF "$@"
    cmake --build "$work/$name" --target install --parallel "$jobs"
}

if wants x264 && [[ ! -f $work/.x264 ]]; then
    mkdir -p "$work/x264"
    (
        cd "$work/x264"
        "$src/x264/configure" --prefix="$prefix" --host="$triple" \
            --cross-prefix="$triple-" --enable-static --disable-cli \
            --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash \
            --extra-cflags="$build_flags"
        make -j "$jobs" install
    )
    touch "$work/.x264"
fi

if wants x265 && [[ ! -f $work/.x265 ]]; then
    if [[ ! -f $work/x265-source/CMakeLists.txt ]]; then
        cp -R "$src/x265/source" "$work/x265-source"
        sed -i -e 's/SET CMP0025 OLD/SET CMP0025 NEW/' -e 's/SET CMP0054 OLD/SET CMP0054 NEW/' \
            "$work/x265-source/CMakeLists.txt"
    fi
    cp "$src/x265/x265Version.txt" "$work/x265Version.txt"
    x265_custom=()
    if [[ $arch == x64 ]]; then
        high=$(cygpath -m "$work/x265-install-high")
        build_cmake x265-main10 "$work/x265-source" "$high" \
            -DENABLE_CLI=OFF -DENABLE_SHARED=OFF -DENABLE_ASSEMBLY=ON \
            -DHIGH_BIT_DEPTH=ON -DEXPORT_C_API=OFF
        x265_custom=(-DLINKED_10BIT=ON -DEXTRA_LIB="$high/lib/libx265.a")
    fi
    build_cmake x265-main8 "$work/x265-source" "$prefix_win" \
        -DENABLE_CLI=OFF -DENABLE_SHARED=OFF -DENABLE_ASSEMBLY=ON "${x265_custom[@]}"
    if [[ $arch == x64 ]]; then
        # CMake builds the Main10 and Main8 objects separately. Merge both
        # into the archive found by FFmpeg's static pkg-config lookup.
        (
            cd "$prefix/lib"
            mv libx265.a libx265-main8.a
            printf 'create libx265.a\naddlib libx265-main8.a\naddlib %s/lib/libx265.a\nsave\nend\n' "$high" | "$AR" -M
            rm libx265-main8.a
        )
    fi
    if [[ ! -f $prefix/lib/pkgconfig/x265.pc ]]; then
        mkdir -p "$prefix/lib/pkgconfig"
        cat > "$prefix/lib/pkgconfig/x265.pc" <<EOF
prefix=$prefix_win
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: x265
Description: H.265/HEVC Encoder
Version: 4.0
Libs: -L\${libdir} -lx265
Libs.private: -lc++ -lc++abi
Cflags: -I\${includedir}
EOF
    fi
    touch "$work/.x265"
fi

if wants aom && [[ ! -f $work/.aom ]]; then
    build_cmake aom "$src/aom" "$prefix_win" \
        -DAOM_TARGET_CPU="$aom_cpu" -DENABLE_EXAMPLES=OFF -DENABLE_TOOLS=OFF \
        -DENABLE_DOCS=OFF -DENABLE_TESTS=OFF -DCONFIG_AV1_DECODER=1 -DCONFIG_AV1_ENCODER=1
    touch "$work/.aom"
fi

if wants vpx && [[ ! -f $work/.vpx ]]; then
    mkdir -p "$work/vpx"
    (
        cd "$work/vpx"
        CROSS="$triple-" "$src/vpx/configure" --target="$vpx_target" --prefix="$prefix" \
            --disable-examples --disable-tools --disable-docs --disable-unit-tests \
            --enable-vp8 --enable-vp9 --enable-vp9-highbitdepth --as=nasm \
            --extra-cflags="$build_flags"
        make -j "$jobs" install
    )
    touch "$work/.vpx"
fi

if wants lame && [[ ! -f $work/.lame ]]; then
    mkdir -p "$work/lame"
    (
        cd "$work/lame"
        CFLAGS="-O2 $build_flags $lame_cflags" "$src/lame/configure" --host="$triple" --prefix="$prefix" \
            --disable-shared --enable-static --disable-frontend --disable-decoder --disable-gtktest
        make -j "$jobs" install
    )
    touch "$work/.lame"
fi

if wants opus && [[ ! -f $work/.opus ]]; then
    build_cmake opus "$src/opus" "$prefix_win"
    touch "$work/.opus"
fi

if wants ogg && [[ ! -f $work/.ogg ]]; then
    build_cmake ogg "$src/ogg" "$prefix_win"
    touch "$work/.ogg"
fi

if wants vorbis && [[ ! -f $work/.vorbis ]]; then
    build_cmake vorbis "$src/vorbis" "$prefix_win"
    touch "$work/.vorbis"
fi

if wants webp && [[ ! -f $work/.webp ]]; then
    build_cmake webp "$src/webp" "$prefix_win" \
        -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
    touch "$work/.webp"
fi

if wants zlib && [[ ! -f $work/.zlib ]]; then
    build_cmake zlib "$src/zlib" "$prefix_win" -DZLIB_BUILD_EXAMPLES=OFF -DZLIB_BUILD_SHARED=OFF
    touch "$work/.zlib"
fi

if ! wants ffmpeg; then
    exit 0
fi

if [[ -f $codec_prefix/lib/libzlibstatic.a && ! -f $codec_prefix/lib/libz.a ]]; then
    cp "$codec_prefix/lib/libzlibstatic.a" "$codec_prefix/lib/libz.a"
fi

# Link x265's C++ runtime from static archives so the FFmpeg DLLs remain
# self-contained and the MinGW import library cannot conflict with libc++abi.
runtime=$(cygpath -m "$toolchain/../$triple/lib")
sed -i "s|^Libs.private:.*|Libs.private: $runtime/libc++.a $runtime/libunwind.a|" \
    "$codec_prefix/lib/pkgconfig/x265.pc"

mkdir -p "$work/ffmpeg"
(
    cd "$work/ffmpeg"
    encoders=libx264,libx265,libaom_av1,libvpx_vp8,libvpx_vp9,mpeg4,mpeg2video,wmv2
    encoders+=,aac,alac,libmp3lame,mp2,wmav2,flac,pcm_s16le,pcm_s16be,libvorbis,libopus,ac3
    encoders+=,mjpeg,png,apng,libwebp,bmp,tiff,targa,gif,wrapped_avframe
    decoders=h264,hevc,libaom_av1,vp6,vp6f,vp6a,vp8,vp9,mpeg1video,mpeg2video,mpeg4,h263,h263p,h263i,flv
    decoders+=,wmv1,wmv2,wmv3,wmv3image,vc1,vc1image,msmpeg4v1,msmpeg4v2,msmpeg4v3,rv10,rv20,rv30,rv40
    decoders+=,dvvideo,prores,svq1,svq3,cinepak,huffyuv,ffv1,theora,rawvideo,utvideo
    decoders+=,aac,aac_latm,alac,mp1,mp2,mp3,mp3float,wmav1,wmav2,wmapro,wmalossless,wmavoice
    decoders+=,flac,vorbis,opus,ac3,eac3,dca,truehd,mlp,amrnb,amrwb,cook,ra_144,ra_288,sipr,qdm2,ape,wavpack
    decoders+=,pcm_s8,pcm_u8,pcm_s16be,pcm_s16le,pcm_u16be,pcm_u16le,pcm_s24be,pcm_s24le,pcm_s24daud
    decoders+=,pcm_s32be,pcm_s32le,pcm_s64be,pcm_s64le,pcm_f32be,pcm_f32le,pcm_f64be,pcm_f64le
    decoders+=,pcm_alaw,pcm_mulaw,pcm_dvd,pcm_bluray,adpcm_ima_wav,adpcm_ms,adpcm_swf
    decoders+=,mjpeg,mjpegb,jpeg2000,png,apng,webp,bmp,tiff,targa,gif,qtrle,wrapped_avframe
    muxers=mp4,matroska,mov,webm,avi,asf,mpeg1system,mpegts,flv,mp3,adts,ipod,wav,flac,aiff,ogg,ac3
    muxers+=,image2,image2pipe,webp,ico,avif,gif,apng,null
    demuxers=mov,matroska,avi,asf,mpegps,mpegts,mpegtsraw,mpegvideo,flv,rm,mp3,aac,aiff,flac,wav,w64,ogg,ac3,eac3
    demuxers+=,image2,image2pipe,image_jpeg_pipe,image_png_pipe,image_webp_pipe,image_bmp_pipe,image_tiff_pipe,ico,gif,apng
    demuxers+=,h264,hevc,av1,obu,ivf,h263,m4v,amr,mxf,ape,wv,aiff,caf,vc1,loas,nut
    filters=scale,pad,format,aformat,aresample,rotate,hflip,vflip,transpose,setpts,asetpts,fps,palettegen,paletteuse
    filters+=,trim,atrim,null,anull,split,asplit,buffer,abuffer,buffersink,abuffersink,xstack,crop
    parsers=aac,aac_latm,ac3,av1,dca,flac,gif,h263,h264,hevc,mjpeg,mpeg4video,mpegvideo,mpegaudio,opus,png,vc1,vorbis,vp8,vp9,webp
    bsfs=aac_adtstoasc,av1_frame_merge,av1_frame_split,av1_metadata,dump_extradata,extract_extradata
    bsfs+=,h264_mp4toannexb,hevc_mp4toannexb,h264_metadata,hevc_metadata,mpeg4_unpack_bframes
    bsfs+=,mjpeg2jpeg,mov2textsub,text2movsub,null,opus_metadata,vp9_superframe,vp9_superframe_split

    "$src/ffmpeg/configure" --prefix="$prefix" --arch="$ff_arch" --target-os=mingw32 \
        --enable-cross-compile --cross-prefix="$triple-" \
        --cc="$CC" --cxx="$CXX" --ld="$CC" --ar="$AR" --ranlib="$RANLIB" --pkg-config="$PKG_CONFIG" \
        --pkg-config-flags=--static --extra-cflags="$build_flags -I$codec_win/include" \
        --extra-ldflags="-static-libgcc -L$codec_win/lib -Wl,--major-subsystem-version,6,--minor-subsystem-version,1" \
        --enable-gpl --enable-libx264 --enable-libx265 --enable-libaom --enable-libvpx \
        --enable-libmp3lame --enable-libopus --enable-libvorbis --enable-libwebp --enable-zlib \
        --enable-w32threads --disable-autodetect --disable-network --disable-avdevice \
        --enable-shared --disable-static --disable-doc --disable-debug --disable-ffplay \
        --disable-everything --enable-protocol=file,pipe \
        --enable-encoder="$encoders" --enable-decoder="$decoders" \
        --enable-muxer="$muxers" --enable-demuxer="$demuxers" --enable-filter="$filters" \
        --enable-parser="$parsers" --enable-bsf="$bsfs" "${hardware[@]}"
    make -j "$jobs" install
)

cp "$prefix/bin/ffmpeg.exe" "$prefix/bin/ffprobe.exe" "$prefix"/bin/{av*,sw*,postproc-*}.dll "$stage/"
cp "$toolchain/../$triple/bin/libwinpthread-1.dll" "$stage/"
"$stage/ffmpeg.exe" -hide_banner -encoders > "$work/encoders.txt"
"$stage/ffmpeg.exe" -hide_banner -decoders > "$work/decoders.txt"
printf '\nMedia engines for %s are ready in %s\n' "$arch" "$stage"
