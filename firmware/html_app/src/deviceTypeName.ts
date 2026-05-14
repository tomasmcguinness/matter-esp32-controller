const DEVICE_TYPE_NAMES: Record<number, string> = {
  0x000e: 'Aggregator',
  0x0011: 'Power Source',
  0x0012: 'OTA Requestor',
  0x0013: 'Bridged Node',
  0x0016: 'Root Node',
  0x0100: 'On/Off Light',
  0x0101: 'Dimmable Light',
  0x010a: 'On/Off Plug',
  0x0302: 'Temperature Sensor',
}

export function deviceTypeName(id: number): string {
  return DEVICE_TYPE_NAMES[id] ?? `0x${id.toString(16).toUpperCase()}`
}

export function deviceTypeIcon(id: number): string {
  switch (id) {
    case 0x0100: return '💡'
    case 0x0101: return '💡'
    case 0x010a: return '🔌'
    default: return '📦'
  }
}
