# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.
import bcb
import pytest


def test_open_missing_file_raises():
    with pytest.raises(bcb.BcbError) as ei:
        bcb.Prior("/no/such/file.bcb-prior")
    assert ei.value.name == "BCB_ERR_INVALID_PRIOR"


def test_bad_prior_bytes_raise():
    with pytest.raises(bcb.BcbError):
        bcb.Prior.from_bytes(b"not a real prior image")


def test_corrupted_payload_detected(prior_path, messages):
    with bcb.Prior(prior_path) as p:
        msg = messages[2]
        comp = bytearray(p.compress(msg, checksum=True))
        comp[-1] ^= 0xFF                       # flip a payload byte
        with pytest.raises(bcb.BcbError) as ei:
            p.decompress(bytes(comp), len(msg))
        assert ei.value.name == "BCB_ERR_CORRUPTED"


def test_output_too_small(prior_path, messages):
    with bcb.Prior(prior_path) as p:
        msg = messages[4]
        comp = p.compress(msg)
        with pytest.raises(bcb.BcbError) as ei:
            p.decompress(comp, original_len=1)     # capacity too small
        assert ei.value.name == "BCB_ERR_OUTPUT_TOO_SMALL"


def test_use_after_close(prior_path):
    p = bcb.Prior(prior_path)
    p.close()
    p.close()                                   # idempotent
    with pytest.raises(ValueError):
        p.compress(b"x")


def test_error_has_code_and_message():
    err = bcb.BcbError(-3)
    assert err.code == -3
    assert err.name == "BCB_ERR_CORRUPTED"
    assert "corrupt" in str(err).lower()
