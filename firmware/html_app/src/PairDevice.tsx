import { useState } from 'react'
import { useNavigate } from 'react-router'

function PairDevice() {
  const navigate = useNavigate()
  const [code, setCode] = useState('')
  const [pairing, setPairing] = useState(false)
  const [error, setError] = useState<string | null>(null)

  function handleSubmit(e: React.FormEvent) {
    e.preventDefault()
    const payload = code.trim()
    if (!payload) return
    setPairing(true)
    setError(null)
    fetch('/controller/commission', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ onboardingPayload: payload }),
    })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<{ nodeId: number }> })
      .then(() => navigate('/devices'))
      .catch((err: unknown) => {
        setError(err instanceof Error ? err.message : 'Commissioning failed')
        setPairing(false)
      })
  }

  return (
    <>
      <div className="mt-3 mb-2">
        <h1>Pair a device</h1>
      </div>
      <hr />
      <p className="text-muted">
        Enter the device's pairing code (QR payload starting <code>MT:</code> or an 11/21-digit manual
        code). The device is commissioned over Bluetooth LE and joined to the Wi-Fi network.
      </p>
      {error && <div className="alert alert-danger">{error}</div>}
      <form onSubmit={handleSubmit} style={{ maxWidth: 480 }}>
        <div className="mb-3">
          <label className="form-label">Pairing code</label>
          <input
            type="text"
            className="form-control"
            placeholder="MT:… or 1234-567-8901"
            value={code}
            onChange={e => setCode(e.target.value)}
            disabled={pairing}
            autoFocus
          />
        </div>
        <div className="d-flex gap-2">
          <button type="submit" className="btn btn-primary" disabled={pairing || !code.trim()}>
            {pairing ? 'Pairing…' : 'Pair device'}
          </button>
          <button type="button" className="btn btn-secondary" onClick={() => navigate('/devices')} disabled={pairing}>
            Cancel
          </button>
        </div>
        {pairing && (
          <div className="alert alert-info mt-3">
            Commissioning in progress — this can take up to a minute. Please keep the device powered and nearby.
          </div>
        )}
      </form>
    </>
  )
}

export default PairDevice
