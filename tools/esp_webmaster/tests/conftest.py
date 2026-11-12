import shutil
import sys
import uuid
from pathlib import Path

import pytest

# 讓 tests/ 可以直接 import 上層模組（nc4 / link / files / sim …）
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# 唔用 pytest 的 tmp_path / tempfile.mkdtemp：Windows sandbox 會為佢加一個
# 「只俾建立者」的 ACL，之後連 scandir / 建子資料夾都 WinError 5。
# 自己用普通 mkdir（預設 ACL）就冇事。
_TMP_ROOT = Path(__file__).resolve().parents[1] / ".testtmp"


@pytest.fixture()
def workdir():
    _TMP_ROOT.mkdir(exist_ok=True)
    d = _TMP_ROOT / ("case-" + uuid.uuid4().hex)
    d.mkdir()
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)
