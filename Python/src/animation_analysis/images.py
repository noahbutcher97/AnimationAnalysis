"""Optional image decoder for pixel-based analysis; no files are exported."""
import base64
import io


def decode_frame(frame, expected_size=None):
    # Image decoding is optional; the library's measurement API accepts packed RGB.
    from PIL import Image
    image = Image.open(io.BytesIO(base64.b64decode(frame["image"].partition(",")[2], validate=True)))
    if image.size != (frame["width"], frame["height"]):
        raise ValueError("Embedded image and recorded dimensions disagree")
    if expected_size is not None and image.size != tuple(expected_size):
        raise ValueError("Image dimensions are outside this detector profile")
    return image.convert("RGB").tobytes()


