# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.
"""pytest fixtures: build two real priors (from the repo corpus) for the binding
tests, using the repo's bcb-prior-build tool."""
import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
CORPUS = REPO / "tests" / "corpus" / "pride_and_prejudice.txt"


def _build_tool():
    tool = REPO / "build" / "bcb-prior-build"
    if not tool.exists():
        subprocess.run(["make", "build/bcb-prior-build"], cwd=REPO, check=True,
                       stdout=subprocess.DEVNULL)
    return tool


@pytest.fixture(scope="session")
def prior_path(tmp_path_factory):
    tool = _build_tool()
    out = tmp_path_factory.mktemp("prior") / "text.bcb-prior"
    subprocess.run([str(tool), str(CORPUS), str(out), "--train-size", "50000",
                    "--landmark-k", "256"], check=True, stdout=subprocess.DEVNULL)
    return out


@pytest.fixture(scope="session")
def other_prior_path(tmp_path_factory):
    """A *different* prior (different train slice) → different prior id."""
    tool = _build_tool()
    out = tmp_path_factory.mktemp("prior2") / "other.bcb-prior"
    # train on a later slice so the id differs
    data = CORPUS.read_bytes()
    alt = tmp_path_factory.mktemp("corpus") / "alt.txt"
    alt.write_bytes(data[len(data) // 2:])
    subprocess.run([str(tool), str(alt), str(out), "--train-size", "50000",
                    "--landmark-k", "256"], check=True, stdout=subprocess.DEVNULL)
    return out


@pytest.fixture(scope="session")
def messages():
    data = CORPUS.read_bytes()
    # a spread of realistic small/medium messages
    return [
        data[1000:1064],
        data[2000:2256],
        data[5000:5512],
        b"",                       # empty
        data[10000:11024],
        b"GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n",
    ]
