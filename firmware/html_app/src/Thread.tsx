import { useEffect, useState } from 'react'

export type ThreadCredentials = {
  hasCredentials: boolean
  networkName?: string
  channel?: number
  panId?: number
  extPanId?: string
  fetchedAt?: number
  borderAgentHost?: string
}

export type BorderAgent = {
  host: string
  ip: string
  port: number
  networkName?: string
}

function Thread() {
  const [creds, setCreds] = useState<ThreadCredentials | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  const [agents, setAgents] = useState<BorderAgent[]>([])
  const [discovering, setDiscovering] = useState(false)
  const [selected, setSelected] = useState<BorderAgent | null>(null)
  const [otpc, setOtpc] = useState('')
  const [fetching, setFetching] = useState(false)
  const [fetchError, setFetchError] = useState<string | null>(null)

  function loadCredentials() {
    fetch('/api/thread/credentials')
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<ThreadCredentials> })
      .then(data => {
        setCreds(data)
        setLoading(false)
      })
      .catch((e: unknown) => {
        setError(e instanceof Error ? e.message : 'Failed to load Thread credentials')
        setLoading(false)
      })
  }

  useEffect(() => { loadCredentials() }, [])

  function handleDiscover() {
    setDiscovering(true)
    setFetchError(null)
    fetch('/api/thread/borderagents')
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<{ agents: BorderAgent[] }> })
      .then(data => {
        setAgents(data.agents)
        if (data.agents.length > 0) setSelected(data.agents[0])
        setDiscovering(false)
      })
      .catch((e: unknown) => {
        setFetchError(e instanceof Error ? e.message : 'Discovery failed')
        setDiscovering(false)
      })
  }

  function handleFetch() {
    if (!selected) return
    setFetching(true)
    setFetchError(null)
    fetch('/api/thread/credentials/fetch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: selected.host, port: selected.port, otpc }),
    })
      .then(async r => {
        if (!r.ok) {
          const msg = await r.json().catch(() => null) as { error?: string } | null
          throw new Error(msg?.error ?? `HTTP ${r.status}`)
        }
        return r.json() as Promise<ThreadCredentials>
      })
      .then(data => {
        setCreds(data)
        setOtpc('')
        setFetching(false)
      })
      .catch((e: unknown) => {
        setFetchError(e instanceof Error ? e.message : 'Fetch failed')
        setFetching(false)
      })
  }

  function handleClear() {
    if (!window.confirm('Clear the stored Thread credentials?')) return
    fetch('/api/thread/credentials', { method: 'DELETE' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => setCreds({ hasCredentials: false }))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Clear failed'))
  }

  if (loading) return <p className="mt-3">Loading…</p>

  return (
    <>
      <div className="mt-3 mb-2">
        <h1>Thread</h1>
      </div>
      <hr />
      {error && <div className="alert alert-danger">{error}</div>}

      <h5 className="mb-2">Stored credentials</h5>
      {creds?.hasCredentials ? (
        <div style={{ border: '1px solid #dee2e6', borderRadius: 6, overflow: 'hidden' }}>
          <div style={{ background: '#f8f9fa', borderBottom: '1px solid #dee2e6', padding: '8px 12px', display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{ fontSize: 20 }}>🧵</span>
            <strong>{creds.networkName || 'Thread network'}</strong>
          </div>
          <div style={{ padding: '8px 12px' }}>
            <dl className="row mb-0" style={{ fontSize: '0.9rem' }}>
              <dt className="col-sm-3">Channel</dt>
              <dd className="col-sm-9">{creds.channel ?? '—'}</dd>
              <dt className="col-sm-3">PAN ID</dt>
              <dd className="col-sm-9">{creds.panId != null ? `0x${creds.panId.toString(16).toUpperCase()}` : '—'}</dd>
              <dt className="col-sm-3">Ext PAN ID</dt>
              <dd className="col-sm-9">{creds.extPanId ?? '—'}</dd>
              <dt className="col-sm-3">Fetched</dt>
              <dd className="col-sm-9">{creds.fetchedAt ? new Date(creds.fetchedAt * 1000).toLocaleString() : '—'}</dd>
              {creds.borderAgentHost && <>
                <dt className="col-sm-3">From</dt>
                <dd className="col-sm-9">{creds.borderAgentHost}</dd>
              </>}
            </dl>
          </div>
          <div style={{ padding: '8px 12px', borderTop: '1px solid #dee2e6' }} className="d-flex gap-2">
            <button className="btn btn-danger btn-sm" onClick={handleClear}>Clear</button>
          </div>
        </div>
      ) : (
        <div className="alert alert-info">No Thread credentials have been shared yet.</div>
      )}

      <h5 className="mt-4 mb-2">Share credentials from a Border Router</h5>
      <p className="text-muted" style={{ fontSize: '0.9rem' }}>
        Put your Thread 1.4 Border Router into credential-sharing mode to get its one-time
        passcode, discover it below, then enter the passcode to pull the network credentials.
      </p>

      {fetchError && <div className="alert alert-danger">{fetchError}</div>}

      <div className="mb-2">
        <button className="btn btn-primary btn-sm" onClick={handleDiscover} disabled={discovering}>
          {discovering ? 'Discovering…' : 'Discover border routers'}
        </button>
      </div>

      {agents.length > 0 && (
        <div className="d-flex flex-column gap-2 mb-3">
          {agents.map(a => (
            <label
              key={`${a.host}:${a.port}`}
              style={{ border: '1px solid #dee2e6', borderRadius: 6, padding: '8px 12px', cursor: 'pointer', display: 'flex', alignItems: 'center', gap: 8 }}
            >
              <input
                type="radio"
                name="borderAgent"
                checked={selected?.host === a.host && selected?.port === a.port}
                onChange={() => setSelected(a)}
              />
              <span>
                <strong>{a.networkName || a.host}</strong>
                <span className="text-muted ms-2" style={{ fontSize: '0.8rem' }}>{a.ip}:{a.port}</span>
              </span>
            </label>
          ))}

          <div className="d-flex gap-2 align-items-center mt-1">
            <input
              type="text"
              className="form-control form-control-sm"
              style={{ maxWidth: 220 }}
              placeholder="One-time passcode"
              value={otpc}
              onChange={e => setOtpc(e.target.value)}
            />
            <button
              className="btn btn-success btn-sm"
              onClick={handleFetch}
              disabled={fetching || !selected || otpc.length === 0}
            >
              {fetching ? 'Fetching…' : 'Fetch credentials'}
            </button>
          </div>
        </div>
      )}
    </>
  )
}

export default Thread
