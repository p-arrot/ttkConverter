"""Create distributable synthetic fixtures and exercise a staged or installed program."""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile
from xml.etree import ElementTree as ET


VIDEO = {
    "mp4-h264": "h264", "mp4-h265": "hevc", "mp4-av1": "av1",
    "mkv-h264": "h264", "mkv-h265": "hevc", "mkv-av1": "av1",
    "mov-h264": "h264", "mov-h265": "hevc", "webm-vp8": "vp8",
    "webm-vp9": "vp9", "webm-av1": "av1", "avi": "mpeg4",
    "wmv": "wmv2", "mpg": "mpeg2video", "ts": "h264", "flv": "h264",
}
AUDIO = {
    "mp3": "mp3", "aac": "aac", "m4a": "aac", "m4a-alac": "alac",
    "wav": "pcm_s16le", "flac": "flac", "aiff": "pcm_s16be",
    "ogg": "vorbis", "opus": "opus", "ac3": "ac3",
}
IMAGE = {
    "jpg": "mjpeg", "png": "png", "webp": "webp", "bmp": "bmp", "tiff": "tiff",
    "tga": "targa", "ico": "png", "avif": "av1", "gif": "gif", "apng": "apng",
}
W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"


def run(*args, timeout=120):
    process = subprocess.run(
        [str(arg) for arg in args], capture_output=True, encoding="utf-8", errors="replace", timeout=timeout,
    )
    if process.returncode:
        raise RuntimeError(f"{args[:4]} returned {process.returncode}\n{process.stdout}\n{process.stderr}")
    return process.stdout.strip()


def make_fixtures(directory, ffmpeg):
    from PIL import Image, ImageDraw, ImageFont
    from reportlab.lib.pagesizes import A4, landscape
    from reportlab.lib.utils import ImageReader
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.cidfonts import UnicodeCIDFont
    from reportlab.pdfgen import canvas

    directory.mkdir(parents=True, exist_ok=True)
    video = directory / "测试 视频.mp4"
    run(ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "testsrc2=size=320x240:rate=25", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-t", "1.2", "-c:v", "libx264", "-threads", "2", "-c:a", "aac", video)
    image = Image.new("RGBA", (128, 96), (0, 0, 0, 0))
    ImageDraw.Draw(image).rounded_rectangle((15, 15, 105, 80), 12, fill=(24, 98, 190, 230))
    image.save(directory / "透明 图片.png")
    second = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(second).ellipse((28, 18, 90, 78), fill=(210, 100, 20, 255))
    image.save(directory / "动画 图片.gif", save_all=True, append_images=[second], duration=[180, 310], loop=0)

    pdfmetrics.registerFont(UnicodeCIDFont("STSong-Light"))
    scan = Image.new("RGB", (1275, 500), "white")
    draw = ImageDraw.Draw(scan)
    fonts = Path(os.environ["WINDIR"]) / "Fonts"
    latin_font = fonts / "arial.ttf"
    chinese_font = Path(os.environ.get("TTK_FIXTURE_CHINESE_FONT", str(fonts / "msyh.ttc")))
    if not chinese_font.is_file():
        chinese_font = fonts / "simsun.ttc"
    if not chinese_font.is_file():
        raise FileNotFoundError("Set TTK_FIXTURE_CHINESE_FONT to a Simplified Chinese TrueType font, or use existing fixtures.")
    draw.text((65, 62), "ORANGE CAT", font=ImageFont.truetype(latin_font, 64), fill="black")
    draw.text((65, 190), "简体中文扫描样本", font=ImageFont.truetype(chinese_font, 62), fill="black")
    draw.text((65, 320), "Local conversion and editable words", font=ImageFont.truetype(latin_font, 45), fill="black")

    pdf = canvas.Canvas(str(directory / "可编辑文本.pdf"), pagesize=A4)
    pdf.setFont("Helvetica", 18)
    pdf.drawString(42, 770, "Editable PDF sample")
    pdf.setFont("STSong-Light", 16)
    pdf.drawString(42, 730, "这是简体中文文字测试。")
    pdf.setFont("Helvetica", 14)
    for index, row in enumerate((("Item", "Count", "Total"), ("Apple", "2", "12"), ("Orange", "4", "20"))):
        for x, value in zip((42, 210, 340), row):
            pdf.drawString(x, 675 - 25 * index, value)
    pdf.save()
    pdf = canvas.Canvas(str(directory / "中英扫描.pdf"), pagesize=A4)
    pdf.drawImage(ImageReader(scan), 35, 560, width=525, height=205)
    pdf.save()

    pdf = canvas.Canvas(str(directory / "混合页面.pdf"), pagesize=A4)
    pdf.setFont("Helvetica", 22)
    pdf.drawString(42, 770, "Embedded Text")
    pdf.showPage()
    pdf.drawImage(ImageReader(scan), 35, 560, width=525, height=205)
    pdf.save()

    pdf = canvas.Canvas(str(directory / "版式样本.pdf"), pagesize=A4)
    pdf.setFont("Helvetica-Bold", 22)
    pdf.setFillColorRGB(.14, .31, .52)
    pdf.drawCentredString(A4[0] / 2, 760, "Centered PDF Title")
    pdf.setFillColorRGB(0, 0, 0)
    pdf.setFont("Helvetica", 12)
    pdf.drawString(50, 710, "This first paragraph has an editable text layer.")
    pdf.setFont("Helvetica-Oblique", 12)
    pdf.drawString(50, 670, "An italic line follows with a visible paragraph gap.")
    pdf.showPage()
    pdf.setPageSize(landscape(A4))
    pdf.drawImage(ImageReader(scan), 35, 300, width=525, height=205)
    pdf.save()


def streams(probe, path):
    return json.loads(run(probe, "-v", "error", "-show_streams", "-of", "json", path))["streams"]


def convert(app, preset, source, output, *options):
    result = Path(run(app, "--convert", preset, source, output, *options))
    if not result.is_file() or result.stat().st_size == 0:
        raise AssertionError(f"Missing result: {result}")
    return result


def read_docx(path):
    with zipfile.ZipFile(path) as document:
        if document.testzip():
            raise AssertionError(f"Bad DOCX ZIP {path}")
        for name in ("[Content_Types].xml", "_rels/.rels"):
            ET.fromstring(document.read(name))
        body = ET.fromstring(document.read("word/document.xml"))
        return "\n".join(node.text for node in body.iter(W + "t") if node.text), body


def check_pdf_layout(app, source, output, *, landscape_second=False):
    path = convert(app, "pdf-docx", source, output, "--pdf-mode", "layout", "--page-dpi", "150")
    _, body = read_docx(path)
    wp = "{http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing}"
    assert len(list(body.iter(wp + "anchor"))) == 2
    sections = [section.find(W + "pgSz") for section in body.iter(W + "sectPr")]
    assert len(sections) == 2
    page_sizes = [(int(size.get(W + "w")), int(size.get(W + "h"))) for size in sections]
    assert page_sizes == [(11906, 16838), (16838, 11906) if landscape_second else (11906, 16838)]
    with zipfile.ZipFile(path) as package:
        for page in (1, 2):
            png = package.read(f"word/media/page-{page}.png")
            assert png.startswith(b"\x89PNG\r\n\x1a\n")
            width, height = struct.unpack(">II", png[16:24])
            if page == 2 and landscape_second:
                width, height = height, width
            assert (width, height) in ((1240, 1754), (1241, 1754)), (width, height)
    cancel_output = output / "取消保留版式"
    cancelled = subprocess.run(
        [str(app), "--convert", "pdf-docx", str(source), str(cancel_output), "--pdf-mode",
         "layout", "--cancel-on-progress"], capture_output=True, timeout=120,
    )
    assert cancelled.returncode == 130 and not (
        cancel_output.exists() and any(cancel_output.iterdir())
    ), cancelled.stderr
    print("PASS original PDF layout uses one full-page image per PDF page", flush=True)


def check_editable_layout(app, source, output):
    path = convert(app, "pdf-docx", source, output, "--pdf-mode", "editable")
    _, body = read_docx(path)
    title = next(paragraph for paragraph in body.iter(W + "p")
                 if "Centered PDF Title" in "".join(t.text or "" for t in paragraph.iter(W + "t")))
    assert title.find(f"{W}pPr/{W}jc").get(W + "val") == "center"
    assert title.find(f".//{W}rPr/{W}b") is not None
    assert title.find(f".//{W}rPr/{W}sz").get(W + "val") == "44"
    assert body.find(f".//{W}rPr/{W}i") is not None
    print("PASS editable PDF layout retains centered heading, sizing, bold and italics", flush=True)


def check_video_settings(app, probe, source, directory):
    details = dict(line.split("\t", 1) for line in run(app, "--probe", source, "--details").splitlines()[1:])
    assert details == {
        "container": "MP4", "video_codec": "H.264", "audio_codec": "AAC",
        "width": "320", "height": "240", "display_aspect_ratio": "4:3", "has_audio": "1",
    }, details
    print("PASS source container, codecs and dimensions", flush=True)

    out = directory / "分辨率测试"
    original = convert(app, "mp4-h264", source, out, "--fast")
    resized = convert(app, "mp4-h264", source, out, "--fast", "--resolution", "1080")
    reduced = convert(app, "mp4-h264", source, out, "--fast", "--resolution", "144")
    for path, expected in ((original, (320, 240)), (resized, (1440, 1080)), (reduced, (192, 144))):
        stream = next(s for s in streams(probe, path) if s["codec_type"] == "video")
        assert (stream["width"], stream["height"]) == expected, stream
        assert stream["display_aspect_ratio"] == "4:3", stream

    ffmpeg = probe.with_name("ffmpeg.exe")
    for name, filter_graph, resolution, expected in (
        ("16比9 单帧", "crop=320:180", "2160", (3840, 2160)),
        ("竖屏 单帧", "crop=320:180,transpose=clock", "720", (720, 1280)),
    ):
        fixture = directory / (name + ".mp4")
        run(ffmpeg, "-v", "error", "-i", source, "-map", "0:v:0", "-an", "-vf", filter_graph,
            "-frames:v", "1", "-c:v", "libx264", "-preset", "ultrafast", fixture)
        result = convert(app, "mp4-h264", fixture, out, "--fast", "--resolution", resolution)
        stream = streams(probe, result)[0]
        assert (stream["width"], stream["height"]) == expected, stream
        if resolution == "2160":
            for nominal, size in (("240", (426, 240)), ("540", (960, 540))):
                smaller = convert(app, "mp4-h264", fixture, out, "--fast", "--resolution", nominal)
                stream = streams(probe, smaller)[0]
                assert (stream["width"], stream["height"]) == size, stream
            silent_output = directory / "无音轨输出"
            result = subprocess.run([str(app), "--convert", "mp3", str(fixture), str(silent_output)],
                                    capture_output=True, timeout=120)
            assert result.returncode == 1 and not list(silent_output.iterdir()), result.stderr
    print("PASS original, 144p, 240p, 540p, 1080p, 4K and portrait resolution; silent video handled", flush=True)

    high = convert(app, "mp4-h264", source, out, "--quality", "high")
    low = convert(app, "mp4-h264", source, out, "--quality", "low")
    very_low = convert(app, "mp4-h264", source, out, "--quality", "very-low")
    assert high.stat().st_size > low.stat().st_size > very_low.stat().st_size
    for path in (high, low, very_low):
        stream = next(s for s in streams(probe, path) if s["codec_type"] == "video")
        assert (stream["width"], stream["height"]) == (320, 240), stream
    print("PASS high, low and very-low quality change size independently of resolution", flush=True)


def check_other_output_settings(app, probe, fixtures, directory):
    output = directory / "输出选项"
    image = fixtures / "透明 图片.png"
    video = fixtures / "测试 视频.mp4"
    for preset, edge, expected in (("png", 640, (640, 480)), ("ico", 64, (64, 64))):
        selected = convert(app, preset, image, output, "--image-size", str(edge))
        stream = streams(probe, selected)[0]
        assert (stream["width"], stream["height"]) == expected, stream
        assert stream["pix_fmt"] == "rgba", stream

    mp3 = convert(app, "mp3", video, output, "--audio-bitrate", "64", "--sample-rate", "22050")
    encoded = streams(probe, mp3)
    assert len(encoded) == 1 and encoded[0]["codec_name"] == "mp3", encoded
    assert int(encoded[0]["sample_rate"]) == 22050 and int(encoded[0]["bit_rate"]) == 64000, encoded
    mp3 = convert(app, "mp3", video, output, "--audio-bitrate", "320", "--sample-rate", "44100")
    encoded = streams(probe, mp3)[0]
    assert int(encoded["sample_rate"]) == 44100 and int(encoded["bit_rate"]) == 320000, encoded
    incompatible_output = directory / "不兼容码率"
    rejected = subprocess.run(
        [str(app), "--convert", "mp3", str(video), str(incompatible_output),
         "--audio-bitrate", "320", "--sample-rate", "22050"],
        capture_output=True, timeout=120,
    )
    assert rejected.returncode == 1 and not (
        incompatible_output.exists() and any(incompatible_output.iterdir())
    ), rejected.stderr
    high = convert(app, "mp3", video, output, "--quality", "high", "--sample-rate", "44100")
    very_low = convert(app, "mp3", video, output, "--quality", "very-low", "--sample-rate", "44100")
    assert int(streams(probe, high)[0]["bit_rate"]) > int(streams(probe, very_low)[0]["bit_rate"])
    wav = convert(app, "wav", video, output, "--sample-rate", "44100")
    assert int(streams(probe, wav)[0]["sample_rate"]) == 44100

    for dpi in (200, 400):
        word = convert(app, "pdf-docx", fixtures / "中英扫描.pdf", output, "--ocr-dpi", str(dpi))
        text, _ = read_docx(word)
        assert "ORANGECAT" in "".join(text.split()), text
    print("PASS image long edge, square ICO, compatible audio bitrate/sample rate and OCR DPI presets", flush=True)


def smoke(app, directory, fixtures, exhaustive):
    probe = app.parent / "engines" / "ffprobe.exe"
    output = directory / "转换 结果"
    video = fixtures / "测试 视频.mp4"
    static = fixtures / "透明 图片.png"
    animated = fixtures / "动画 图片.gif"
    if not run(app, "--probe", video).startswith("mp4-h264") or not run(app, "--probe", animated).startswith("apng"):
        raise AssertionError("Media type discovery selected wrong defaults")
    check_video_settings(app, probe, video, directory)
    check_other_output_settings(app, probe, fixtures, directory)
    checks = list(VIDEO) if exhaustive else ["mp4-h264", "mp4-h265", "mp4-av1", "webm-vp9"]
    for preset in checks:
        converted = convert(app, preset, video, output, "--fast")
        actual = streams(probe, converted)
        if not any(s["codec_name"] == VIDEO[preset] for s in actual) or not any(s["codec_type"] == "audio" for s in actual):
            raise AssertionError(f"Bad video streams: {preset} {actual}")
        if preset in ("mp4-h265", "mp4-av1"):
            decoded = convert(app, "mp4-h264", converted, output, "--fast")
            assert streams(probe, decoded)[0]["codec_name"] == "h264"
        print(f"PASS {preset}", flush=True)

    available = {line.split("\t", 1)[0] for line in run(app, "--formats").splitlines()}
    if "mp4-h265-10" in available:
        main10 = convert(app, "mp4-h265-10", video, output, "--fast")
        stream = streams(probe, main10)[0]
        assert stream["codec_name"] == "hevc" and stream["pix_fmt"] == "yuv420p10le", stream
        print("PASS H.265 Main 10", flush=True)

    for preset in list(AUDIO) if exhaustive else ["mp3", "flac", "opus"]:
        converted = convert(app, preset, video, output)
        audio_streams = streams(probe, converted)
        assert audio_streams[0]["codec_name"] == AUDIO[preset], preset
        assert all(stream["codec_type"] == "audio" for stream in audio_streams), audio_streams
        print(f"PASS {preset}", flush=True)

    for preset in list(IMAGE) if exhaustive else ["jpg", "png", "ico", "apng", "avif"]:
        source = animated if preset in ("gif", "apng") else static
        converted = convert(app, preset, source, output)
        stream = streams(probe, converted)[0]
        assert stream["codec_name"] == IMAGE[preset], preset
        if preset == "png":
            assert stream["pix_fmt"] == "rgba", stream
        if preset == "apng":
            from PIL import Image
            with Image.open(converted) as animation:
                assert animation.n_frames == 2
        print(f"PASS {preset}", flush=True)

    remuxed = convert(app, "mkv-h264", video, output, "--remux")
    hash_packets = (probe, "-v", "error", "-select_streams", "v:0", "-show_packets", "-show_entries",
                    "packet=data_hash", "-show_data_hash", "sha256", "-of", "csv=p=0")
    original = run(*hash_packets, video)
    assert original and original == run(*hash_packets, remuxed), "Video packets changed during remux"
    print("PASS lossless remux", flush=True)

    text, body = read_docx(convert(app, "pdf-docx", fixtures / "可编辑文本.pdf", output))
    assert "Editable PDF sample" in text and "文字测试" in text, text
    assert next(body.iter(W + "tbl"), None) is not None, "Expected editable table"
    text, _ = read_docx(convert(app, "pdf-docx", fixtures / "中英扫描.pdf", output))
    normalized = "".join(text.split())
    assert "ORANGECAT" in normalized and "简体中文" in normalized and "样本" in normalized, text
    text, _ = read_docx(convert(app, "pdf-docx", fixtures / "混合页面.pdf", output))
    assert "Embedded Text" in text and "ORANGE" in text, text
    print("PASS PDF text, table, bilingual OCR and mixed pages", flush=True)
    layout_fixture = fixtures / "版式样本.pdf"
    if layout_fixture.is_file():
        check_pdf_layout(app, layout_fixture, output, landscape_second=True)
        check_editable_layout(app, layout_fixture, output)
    else:
        check_pdf_layout(app, fixtures / "混合页面.pdf", output)
    cancelled = subprocess.run(
        [str(app), "--convert", "mp4-av1", str(video), str(directory / "取消测试"), "--cancel-on-progress"],
        capture_output=True, timeout=120,
    )
    assert cancelled.returncode == 130 and not list((directory / "取消测试").iterdir()), cancelled.stderr
    print("PASS cancel without partial output", flush=True)
    assert len(list(output.glob("测试 视频*.mp4"))) >= 3, "Same-name outputs were overwritten"
    assert not any(path.name.startswith(".ttk-") for path in output.iterdir()), "Unexpected temporary output"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path)
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--make-fixtures", action="store_true")
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--all-formats", action="store_true")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ttk-smoke-") as temporary:
        directory = Path(temporary)
        fixtures = options.fixtures.resolve() if options.fixtures else directory / "测试 素材"
        app = options.app.resolve() if options.app else None
        if options.make_fixtures or not options.fixtures:
            ffmpeg = options.ffmpeg.resolve() if options.ffmpeg else app.parent / "engines" / "ffmpeg.exe"
            make_fixtures(fixtures, ffmpeg)
        if app:
            smoke(app, directory, fixtures, options.all_formats)


if __name__ == "__main__":
    main()
