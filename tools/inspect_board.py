"""Extract current PCB pad nets from the user's EPro project; no CAD edits."""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
source = next(root.parent.glob('*.epro2'))
with zipfile.ZipFile(source) as project:
    text = project.read(next(n for n in project.namelist() if n.endswith('.epru'))).decode('utf-8')
attributes, nets = {}, {}
in_pcb = False
for line in text.splitlines():
    try:
        header, body = (json.loads(s) for s in line.rstrip('|').split('||'))
    except (ValueError, json.JSONDecodeError):
        continue
    kind = header['type']
    if kind == 'DOCHEAD':
        in_pcb = body.get('docType') == 'PCB'
    if not in_pcb:
        continue
    if kind == 'ATTR':
        attributes.setdefault(body.get('parentId'), {})[body.get('key')] = body.get('value')
    if kind == 'PAD_NET':
        _, component, pad, *_ = json.loads(header['id'])
        nets.setdefault(component, {})[pad] = body['padNet']
selected = {}
for component, attr in attributes.items():
    name = attr.get('Designator')
    if name in ['U5', 'U6', 'R8', 'R9', 'R10', 'D1', 'D3', 'U12']:
        selected[name] = {'pcb_component_id': component, 'pads': nets.get(component, {})}
expected = {
    'U6': {'21': 'SPI_CS', '17': 'SPI_CLK', '19': 'SPI_MOSI', '20': 'SPI_MISO',
           '15': 'AD5941_RESET', '14': 'AD5941_INT0', '27': 'PIN_POWER_ON',
           '9': 'PIN_LED_RED', '8': 'PIN_LED_GREEN', '42': 'ADC_BAT_VOLT'},
    'U5': {'15': 'SPI_CS', '16': 'SPI_CLK', '17': 'SPI_MOSI', '18': 'SPI_MISO',
           '19': 'AD5941_INT0', '22': 'AD5941_RESET', '45': 'WE0', '47': 'CE', '48': 'RE', '40': ''},
    'R9': {'2': 'VBAT', '1': 'ADC_BAT_VOLT'},
    'R10': {'2': 'ADC_BAT_VOLT', '1': 'AGND'},
}
checks = 0
for component, pads in expected.items():
    for pad, net in pads.items():
        assert selected[component]['pads'].get(pad) == net, (component, pad, net)
        checks += 1
assert {selected['U5']['pads']['32'], selected['U5']['pads']['33']} == set(selected['R8']['pads'].values())
checks += 1
result = {'source': source.name, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'description': 'CAD logical PAD_NET evidence only; does not prove copper continuity or assembled parts.',
          'checks_passed': checks,
          'resistor_values_from_schematic': {'R8_ohm': 200, 'R9_ohm': 820000, 'R10_ohm': 1000000},
          'components': selected}
(root / 'docs/pcb_connections.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print(f'PASS: {checks} PCB pad-net checks, including SPI, WE0/RE/CE, RCAL and battery divider')
