import { useEffect, useState } from 'react'
import { deviceTypeName, deviceTypeIcon } from './deviceTypeName'

export type CommissionedDevice = {
  nodeId: number
  vendorName: string
  productName: string
  deviceType: number
}

type DevicesResponse = {
  devices: CommissionedDevice[]
}

function Devices() {
  const [devices, setDevices] = useState<CommissionedDevice[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    fetch('/api/devices')
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<DevicesResponse> })
      .then(data => {
        setDevices(data.devices)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to load devices')
        setLoading(false)
      })
  }, [])

  function handleDelete(nodeId: number) {
    if (!window.confirm('Remove this device from the fabric?')) return
    fetch(`/api/devices/${nodeId}`, { method: 'DELETE' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => setDevices(prev => prev.filter(d => d.nodeId !== nodeId)))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Delete failed'))
  }

  if (loading) return <p className="mt-3">Loading…</p>

  return (
    <>
      <div className="mt-3 mb-2">
        <h1>Devices</h1>
      </div>
      <hr />
      {error && <div className="alert alert-danger">{error}</div>}
      {devices.length === 0 ? (
        <div className="alert alert-info">No devices have been commissioned yet.</div>
      ) : (
        <div className="d-flex flex-column gap-2">
          {devices.map(dev => {
            const name = [dev.vendorName, dev.productName].filter(Boolean).join(' ')
            return (
              <div
                key={dev.nodeId}
                style={{ border: '1px solid #dee2e6', borderRadius: 6, overflow: 'hidden' }}
              >
                <div style={{ background: '#f8f9fa', borderBottom: '1px solid #dee2e6', padding: '8px 12px', display: 'flex', alignItems: 'center', gap: 8 }}>
                  <span style={{ fontSize: 20 }}>{deviceTypeIcon(dev.deviceType)}</span>
                  <div>
                    <strong>{name || `Node 0x${dev.nodeId.toString(16).toUpperCase()}`}</strong>
                    {name && (
                      <span className="text-muted ms-2" style={{ fontSize: '0.8rem' }}>
                        Node 0x{dev.nodeId.toString(16).toUpperCase()}
                      </span>
                    )}
                  </div>
                </div>
                <div style={{ padding: '8px 12px' }}>
                  <span className="badge" style={{ background: '#1e40af', color: '#fff', padding: '3px 8px', borderRadius: 10, fontSize: '0.75rem' }}>
                    {deviceTypeName(dev.deviceType)}
                  </span>
                </div>
                <div style={{ padding: '8px 12px', borderTop: '1px solid #dee2e6' }} className="d-flex gap-2">
                  <button className="btn btn-danger btn-sm" onClick={() => handleDelete(dev.nodeId)}>
                    Remove
                  </button>
                </div>
              </div>
            )
          })}
        </div>
      )}
    </>
  )
}

export default Devices
