"""Local, self-contained QR images for temporary connection links."""
import base64

from ._vendor.qrcodegen import QrCode


def qr_data_url(text):
    code = QrCode.encode_text(text, QrCode.Ecc.MEDIUM)
    size = code.get_size()
    # Four light modules on every side remain part of the image at any size.
    paths = ' '.join(f'M{x + 4},{y + 4}h1v1h-1z'
                     for y in range(size) for x in range(size) if code.get_module(x, y))
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size + 8} {size + 8}" '
           f'shape-rendering="crispEdges"><rect width="100%" height="100%" fill="#fff"/>'
           f'<path d="{paths}" fill="#000"/></svg>')
    return 'data:image/svg+xml;base64,' + base64.b64encode(svg.encode('ascii')).decode('ascii')
