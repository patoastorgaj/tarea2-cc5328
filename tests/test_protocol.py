"""Pruebas del lado PC: python -m pytest tests"""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "gui_python"))
import protocol as proto  # noqa: E402


def test_config_command_completo():
    cmd = json.loads(proto.config_command(2, ["P", "T"], True, 10, True, 28.04, 0.3, "c1"))
    assert cmd["type"] == "config"
    assert cmd["variables"] == ["T", "P"]  # orden fijo
    assert cmd["peak"]["threshold_c"] == 28.0
    assert cmd["id"] == "c1"


@pytest.mark.parametrize("sel", [["T"], ["H"], ["P"], ["T", "H"], ["T", "P"], ["H", "P"], ["T", "H", "P"]])
def test_siete_combinaciones(sel):
    cmd = json.loads(proto.config_command(1, sel, False, 10, False, 28.0, 0.0))
    assert cmd["variables"] == sel


def test_seleccion_vacia_rechazada():
    with pytest.raises(ValueError):
        proto.config_command(1, [], True, 10, True, 28.0, 0.3)


def test_frecuencia_invalida_rechazada():
    with pytest.raises(ValueError):
        proto.config_command(3, ["T"], True, 10, True, 28.0, 0.3)


def test_parse_line():
    assert proto.parse_line('{"type":"sample","seq":1,"t_ms":10,"valid":true}')["seq"] == 1
    assert proto.parse_line("ets Jun  8 2016 00:22:57") is None
    assert proto.parse_line('{"type":"desconocido"}') is None
    assert proto.parse_line('{"type":"sample",') is None


def test_promedio_referencia():
    ref = proto.ReferenceAverage()
    for i in range(10):
        ref.add(i * 1000, 20.0 + i)
    mean, n = ref.mean(9000, 5)
    assert n == 5 and mean == pytest.approx(27.0)


# misma serie que usa el autotest del firmware
SERIE = [27.0, 27.4, 28.2, 28.6, 28.4, 28.1, 27.6, 27.9, 28.3,
         28.3, 28.2, 27.5, 29.0, 29.1, 29.1, 29.2, 28.0]


def test_picos_con_histeresis():
    picos = proto.reference_peaks([(i * 1000, v) for i, v in enumerate(SERIE)], 28.0, 0.3)
    assert picos == [(3000, 28.6), (8000, 28.3), (13000, 29.1)]


def test_picos_sin_histeresis():
    picos = proto.reference_peaks([(i * 1000, v) for i, v in enumerate(SERIE)], 28.0, 0.0)
    assert [t for t, _ in picos] == [3000, 8000, 13000, 15000]


def test_subida_monotona_no_es_pico():
    serie = [(i * 1000, 28.0 + 0.1 * i) for i in range(20)]
    assert proto.reference_peaks(serie, 28.0, 0.0) == []


def test_rate_meter():
    r = proto.RateMeter()
    for i in range(10):
        r.add(i * 200)
    assert r.hz() == pytest.approx(5.0)
