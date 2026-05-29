# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.
"""Pythonic bindings for BCB — small-message / fixed-record lossless compression
with a shared learned prior.

    from bcb import Prior
    with Prior("sensors.bcb-prior") as p:
        comp = p.compress(b"...packet...")
        back = p.decompress(comp, original_len=len(original))

Thread-safety: a ``Prior`` is read-only and may be shared across threads. Each
compress/decompress call uses its own transient codec state, and ``Encoder`` /
``Decoder`` instances each hold their own state — but a single Encoder/Decoder
instance must not be used concurrently from multiple threads.
"""
import os

from ._bcb_cffi import ffi, lib

__all__ = ["Prior", "Encoder", "Decoder", "BcbError", "version", "compress_bound",
           "PRIOR_ID_LEN"]

PRIOR_ID_LEN = 16

_STATUS_NAME = {
    0: "BCB_OK",
    -1: "BCB_ERR_INVALID_PRIOR",
    -2: "BCB_ERR_OUTPUT_TOO_SMALL",
    -3: "BCB_ERR_CORRUPTED",
    -4: "BCB_ERR_VERSION",
    -5: "BCB_ERR_PRIOR_ID_MISMATCH",
}


class BcbError(Exception):
    """Raised when a BCB C call returns a negative ``BcbStatus``."""

    def __init__(self, code, detail=None):
        self.code = int(code)
        self.name = _STATUS_NAME.get(self.code, "BCB_ERR_UNKNOWN")
        msg = ffi.string(lib.bcb_strerror(self.code)).decode("utf-8", "replace")
        text = f"{self.name} ({self.code}): {msg}"
        if detail:
            text = f"{text} — {detail}"
        super().__init__(text)


def version() -> str:
    """Library semver string, e.g. ``"0.2.0"``."""
    return ffi.string(lib.bcb_version()).decode("ascii")


def compress_bound(input_len: int) -> int:
    """Worst-case output size for ``input_len`` bytes (for buffer sizing)."""
    return int(lib.bcb_compress_bound(input_len))


def _check(n, detail=None):
    if n < 0:
        raise BcbError(n, detail)
    return n


class Prior:
    """A trained BCB prior (``.bcb-prior``). Open from a file (mmap) or bytes.

    Use as a context manager, or call :meth:`close` when done.
    """

    __slots__ = ("_p",)

    def __init__(self, path=None, *, data=None):
        if (path is None) == (data is None):
            raise ValueError("provide exactly one of `path` or `data=`")
        if path is not None:
            cpath = os.fspath(path)
            if isinstance(cpath, str):
                cpath = cpath.encode("utf-8")
            p = lib.bcb_prior_open(cpath)
            why = f"cannot open prior {path!r}"
        else:
            buf = bytes(data)
            p = lib.bcb_prior_from_memory(buf, len(buf))
            why = "cannot parse prior image"
        if p == ffi.NULL:
            raise BcbError(lib.BCB_ERR_INVALID_PRIOR, why)
        self._p = p

    @classmethod
    def from_bytes(cls, data) -> "Prior":
        """Open a prior from an in-memory image (copied and owned)."""
        return cls(data=data)

    def _handle(self):
        if self._p is None or self._p == ffi.NULL:
            raise ValueError("Prior is closed")
        return self._p

    # ── lifecycle ────────────────────────────────────────────────────────────
    def close(self):
        """Release the prior. Idempotent; safe to call more than once."""
        if getattr(self, "_p", None) not in (None, ffi.NULL):
            lib.bcb_prior_close(self._p)
        self._p = ffi.NULL

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # ── introspection ────────────────────────────────────────────────────────
    @property
    def record_size(self) -> int:
        """Fixed record size (>0) if this prior has a structural schema, else 0."""
        return int(lib.bcb_prior_record_size(self._handle()))

    @property
    def memory_footprint(self) -> int:
        """Approximate memory footprint in bytes."""
        return int(lib.bcb_prior_memory_footprint(self._handle()))

    @property
    def id(self) -> bytes:
        """16-byte prior id (SHA-256 prefix). Same training → same id."""
        out_len = ffi.new("size_t *")
        ptr = lib.bcb_prior_id(self._handle(), out_len)
        if ptr == ffi.NULL:
            return b""
        return bytes(ffi.buffer(ptr, out_len[0]))

    # ── one-shot compress / decompress ───────────────────────────────────────
    def compress(self, data, *, checksum: bool = True, prior_id: bool = False) -> bytes:
        """Compress ``data`` (bytes-like) into a self-describing container.

        ``checksum`` embeds a CRC32 (default on) so corruption raises on decode.
        ``prior_id`` embeds the 16-byte prior id so a wrong prior is detected.
        """
        p = self._handle()
        mv = memoryview(data).cast("B")
        n_in = mv.nbytes
        cap = lib.bcb_compress_bound(n_in)
        out = ffi.new("uint8_t[]", cap)
        e = lib.bcb_encoder_new(p)
        if e == ffi.NULL:
            raise BcbError(lib.BCB_ERR_INVALID_PRIOR, "encoder allocation failed")
        try:
            lib.bcb_encoder_set_checksum(e, 1 if checksum else 0)
            lib.bcb_encoder_set_prior_id(e, 1 if prior_id else 0)
            n = _check(lib.bcb_encode(e, bytes(mv), n_in, out, cap))
        finally:
            lib.bcb_encoder_free(e)
        return bytes(ffi.buffer(out, n))

    def decompress(self, data, original_len: int) -> bytes:
        """Decompress a container. ``original_len`` is the known original size
        (also used as the output capacity). Raises :class:`BcbError` on corruption
        or a mismatched prior."""
        if original_len < 0:
            raise ValueError("original_len must be >= 0")
        p = self._handle()
        mv = memoryview(data).cast("B")
        out = ffi.new("uint8_t[]", original_len if original_len > 0 else 1)
        d = lib.bcb_decoder_new(p)
        if d == ffi.NULL:
            raise BcbError(lib.BCB_ERR_INVALID_PRIOR, "decoder allocation failed")
        try:
            n = _check(lib.bcb_decode(d, bytes(mv), mv.nbytes, out, original_len))
        finally:
            lib.bcb_decoder_free(d)
        return bytes(ffi.buffer(out, n))

    def encoder(self, *, checksum: bool = True, prior_id: bool = False) -> "Encoder":
        """A reusable :class:`Encoder` bound to this prior (per-message reset)."""
        return Encoder(self, checksum=checksum, prior_id=prior_id)

    def decoder(self) -> "Decoder":
        """A reusable :class:`Decoder` bound to this prior."""
        return Decoder(self)


class Encoder:
    """Reusable encoder (one instance = one codec state; not thread-shared)."""

    __slots__ = ("_e",)

    def __init__(self, prior: Prior, *, checksum: bool = True, prior_id: bool = False):
        e = lib.bcb_encoder_new(prior._handle())
        if e == ffi.NULL:
            raise BcbError(lib.BCB_ERR_INVALID_PRIOR, "encoder allocation failed")
        self._e = e
        lib.bcb_encoder_set_checksum(e, 1 if checksum else 0)
        lib.bcb_encoder_set_prior_id(e, 1 if prior_id else 0)

    def encode(self, data) -> bytes:
        if self._e in (None, ffi.NULL):
            raise ValueError("Encoder is closed")
        mv = memoryview(data).cast("B")
        cap = lib.bcb_compress_bound(mv.nbytes)
        out = ffi.new("uint8_t[]", cap)
        n = _check(lib.bcb_encode(self._e, bytes(mv), mv.nbytes, out, cap))
        return bytes(ffi.buffer(out, n))

    def close(self):
        if getattr(self, "_e", None) not in (None, ffi.NULL):
            lib.bcb_encoder_free(self._e)
        self._e = ffi.NULL

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class Decoder:
    """Reusable decoder (one instance = one codec state; not thread-shared)."""

    __slots__ = ("_d",)

    def __init__(self, prior: Prior):
        d = lib.bcb_decoder_new(prior._handle())
        if d == ffi.NULL:
            raise BcbError(lib.BCB_ERR_INVALID_PRIOR, "decoder allocation failed")
        self._d = d

    def decode(self, data, original_len: int) -> bytes:
        if self._d in (None, ffi.NULL):
            raise ValueError("Decoder is closed")
        if original_len < 0:
            raise ValueError("original_len must be >= 0")
        mv = memoryview(data).cast("B")
        out = ffi.new("uint8_t[]", original_len if original_len > 0 else 1)
        n = _check(lib.bcb_decode(self._d, bytes(mv), mv.nbytes, out, original_len))
        return bytes(ffi.buffer(out, n))

    def close(self):
        if getattr(self, "_d", None) not in (None, ffi.NULL):
            lib.bcb_decoder_free(self._d)
        self._d = ffi.NULL

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


try:
    __version__ = version()
except Exception:  # pragma: no cover
    __version__ = "0.2.0"
