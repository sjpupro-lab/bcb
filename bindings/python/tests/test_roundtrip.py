# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.
import bcb
import pytest


def test_version():
    assert bcb.version().count(".") == 2


def test_oneshot_roundtrip_lossless(prior_path, messages):
    with bcb.Prior(prior_path) as p:
        for msg in messages:
            comp = p.compress(msg)
            back = p.decompress(comp, original_len=len(msg))
            assert back == msg


def test_checksum_toggle(prior_path, messages):
    with bcb.Prior(prior_path) as p:
        msg = messages[2]
        with_crc = p.compress(msg, checksum=True)
        no_crc = p.compress(msg, checksum=False)
        # no-CRC is never larger (drops the 4-byte field)
        assert len(no_crc) <= len(with_crc)
        assert p.decompress(no_crc, len(msg)) == msg
        assert p.decompress(with_crc, len(msg)) == msg


def test_compress_bound_respected(prior_path, messages):
    with bcb.Prior(prior_path) as p:
        for msg in messages:
            assert len(p.compress(msg)) <= bcb.compress_bound(len(msg))


def test_encoder_decoder_reuse(prior_path, messages):
    with bcb.Prior(prior_path) as p:
        with p.encoder() as enc, p.decoder() as dec:
            for msg in messages:
                comp = enc.encode(msg)
                assert dec.decode(comp, len(msg)) == msg


def test_from_bytes_matches_file(prior_path, messages):
    image = prior_path.read_bytes()
    with bcb.Prior(prior_path) as pf, bcb.Prior.from_bytes(image) as pm:
        assert pf.id == pm.id
        msg = messages[4]
        assert pf.compress(msg) == pm.compress(msg)        # bit-identical
        assert pm.decompress(pm.compress(msg), len(msg)) == msg


def test_prior_introspection(prior_path):
    with bcb.Prior(prior_path) as p:
        assert len(p.id) == bcb.PRIOR_ID_LEN
        assert p.memory_footprint > 0
        assert p.record_size == 0          # landmark/text prior has no schema


def test_prior_id_mismatch(prior_path, other_prior_path, messages):
    msg = messages[2]
    with bcb.Prior(prior_path) as p, bcb.Prior(other_prior_path) as q:
        assert p.id != q.id
        comp = p.compress(msg, prior_id=True)
        assert p.decompress(comp, len(msg)) == msg          # right prior decodes
        with pytest.raises(bcb.BcbError) as ei:
            q.decompress(comp, len(msg))                     # wrong prior rejected
        assert ei.value.name == "BCB_ERR_PRIOR_ID_MISMATCH"
