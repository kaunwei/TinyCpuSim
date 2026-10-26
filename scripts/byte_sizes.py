"""Shared normalization and conversion for legacy byte-size values."""

import re


def parse_byte_size(value, *, invalid_suffixless="raise"):
    """Convert the supported legacy suffix forms to bytes.

    ``invalid_suffixless="normalized"`` preserves config.py's historical
    behavior for values without a recognized suffix.
    """
    normalized = str(value).strip().upper()
    if normalized.endswith("KB") or normalized.endswith("K"):
        digits = int(re.sub(r"[^0-9]", "", normalized))
        return digits * 1024
    if normalized.endswith("MB") or normalized.endswith("M"):
        digits = int(re.sub(r"[^0-9]", "", normalized))
        return digits * 1024 * 1024
    if normalized.endswith("B"):
        return int(re.sub(r"[^0-9]", "", normalized))
    try:
        return int(normalized)
    except ValueError:
        if invalid_suffixless == "normalized":
            return normalized
        raise
