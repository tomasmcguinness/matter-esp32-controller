import { useEffect, useState, type ReactNode } from 'react'
import { useNavigate } from 'react-router'
import { deviceTypeName, deviceTypeIcon } from './deviceTypeName'

export type DeviceEndpoint = {
  endpointId: number
  parentEndpointId?: number
  label: string
  included: boolean
  deviceTypes: number[]
  parts: number[]
}

export type CommissionedDevice = {
  nodeId: number
  vendorName: string
  productName: string
  name?: string
  deviceType: number
  endpoints?: DeviceEndpoint[]
}

type DevicesResponse = {
  devices: CommissionedDevice[]
}

type AclEntry = {
  privilege: number
  authMode: number
  subjects: string[]
}

type AclResponse = {
  entries: AclEntry[]
}

type BindingTarget = {
  node?: string
  endpoint?: number
  cluster: number
  group?: number
}

type BindingTableResponse = {
  endpoint: number | null
  entries: BindingTarget[]
}

type GroupKeyMapEntry = {
  group: number
  keyset: number
}

type GroupTableEntry = {
  group: number
  endpoints: number[]
  name: string
}

type GroupStateResponse = {
  groupKeyMap: GroupKeyMapEntry[]
  groupTable: GroupTableEntry[]
}

type ThreadInfoResponse = {
  networkName: string | null
  extendedPanId: string | null
  panId: number | null
  channel: number | null
  routingRole: number | null
}

const ROUTING_ROLE_NAMES: Record<number, string> = {
  0: 'Unspecified', 1: 'Unassigned', 2: 'Sleepy End Device', 3: 'End Device', 4: 'REED', 5: 'Router', 6: 'Leader',
}
const PRIVILEGE_NAMES: Record<number, string> = { 1: 'View', 3: 'Operate', 4: 'Manage', 5: 'Administer' }
const AUTH_MODE_NAMES: Record<number, string> = { 1: 'PASE', 2: 'CASE', 3: 'Group' }
const CLUSTER_NAMES: Record<number, string> = { 6: 'On/Off' }
const CONTROLLER_NODE_ID = '112233'

function formatSubject(subject: string): string {
  let hex: string
  try {
    hex = BigInt(subject).toString(16).toUpperCase()
  } catch {
    hex = subject
  }
  return `0x${hex}${subject === CONTROLLER_NODE_ID ? ' (controller)' : ''}`
}

function formatCluster(cluster: number): string {
  return CLUSTER_NAMES[cluster] ?? `0x${cluster.toString(16).toUpperCase()}`
}

function Modal({ title, onClose, children }: { title: string; onClose: () => void; children: ReactNode }) {
  return (
    <div
      onClick={onClose}
      style={{
        position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)',
        display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1050,
      }}
    >
      <div
        onClick={e => e.stopPropagation()}
        style={{
          background: '#fff', borderRadius: 8, width: 'min(560px, 92vw)',
          maxHeight: '85vh', display: 'flex', flexDirection: 'column',
          boxShadow: '0 10px 40px rgba(0,0,0,0.25)',
        }}
      >
        <div style={{ padding: '12px 16px', borderBottom: '1px solid #dee2e6', display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
          <strong>{title}</strong>
          <button type="button" className="btn-close" aria-label="Close" onClick={onClose} />
        </div>
        <div style={{ padding: '12px 16px', overflowY: 'auto' }}>{children}</div>
      </div>
    </div>
  )
}

function Devices() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<CommissionedDevice[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [reinterviewing, setReinterviewing] = useState<number | null>(null)
  const [resettingGroups, setResettingGroups] = useState<number | null>(null)
  const [resettingAcl, setResettingAcl] = useState<number | null>(null)
  const [resettingBindings, setResettingBindings] = useState<number | null>(null)
  const [aclNodeId, setAclNodeId] = useState<number | null>(null)
  const [acl, setAcl] = useState<AclEntry[] | null>(null)
  const [aclLoading, setAclLoading] = useState(false)
  const [aclError, setAclError] = useState<string | null>(null)
  const [bindingsNodeId, setBindingsNodeId] = useState<number | null>(null)
  const [bindings, setBindings] = useState<BindingTarget[] | null>(null)
  const [bindingsLoading, setBindingsLoading] = useState(false)
  const [bindingsError, setBindingsError] = useState<string | null>(null)
  const [groupStateNodeId, setGroupStateNodeId] = useState<number | null>(null)
  const [groupState, setGroupState] = useState<GroupStateResponse | null>(null)
  const [groupStateLoading, setGroupStateLoading] = useState(false)
  const [groupStateError, setGroupStateError] = useState<string | null>(null)
  const [threadInfoNodeId, setThreadInfoNodeId] = useState<number | null>(null)
  const [threadInfo, setThreadInfo] = useState<ThreadInfoResponse | null>(null)
  const [threadInfoLoading, setThreadInfoLoading] = useState(false)
  const [threadInfoError, setThreadInfoError] = useState<string | null>(null)

  function loadDevices() {
    return fetch('/api/devices')
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<DevicesResponse> })
      .then(data => setDevices(data.devices))
  }

  useEffect(() => {
    loadDevices()
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Failed to load devices'))
      .finally(() => setLoading(false))
  }, [])

  function handleDelete(nodeId: number) {
    if (!window.confirm('Remove this device from the fabric?')) return
    fetch(`/api/devices/${nodeId}`, { method: 'DELETE' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => setDevices(prev => prev.filter(d => d.nodeId !== nodeId)))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Delete failed'))
  }

  function handleRename(dev: CommissionedDevice) {
    const input = window.prompt('Enter a name for this device', dev.name ?? '')
    if (input === null) return // cancelled
    const name = input.trim()
    setError(null)
    fetch(`/api/devices/${dev.nodeId}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name }),
    })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => setDevices(prev => prev.map(d => d.nodeId === dev.nodeId ? { ...d, name } : d)))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Rename failed'))
  }

  function handleReinterview(nodeId: number) {
    setError(null)
    setReinterviewing(nodeId)
    fetch(`/api/reinterview/${nodeId}`, { method: 'POST' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .then(() => loadDevices())
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Re-interview failed'))
      .finally(() => setReinterviewing(null))
  }

  function handleResetGroups(nodeId: number) {
    if (!window.confirm('Clear all Matter group state (keysets, group key map, groups) this controller set on the device?')) return
    setError(null)
    setResettingGroups(nodeId)
    fetch(`/api/resetgroups/${nodeId}`, { method: 'POST' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Reset groups failed'))
      .finally(() => setResettingGroups(null))
  }

  function handleResetBindings(nodeId: number) {
    if (!window.confirm('Clear this device\'s Binding table? It will stop sending commands to any bound device or group.')) return
    setError(null)
    setResettingBindings(nodeId)
    fetch(`/api/resetbindings/${nodeId}`, { method: 'POST' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Reset bindings failed'))
      .finally(() => setResettingBindings(null))
  }

  function handleResetAcl(nodeId: number) {
    if (!window.confirm('Reset this device\'s ACL to only the controller (Administer) entry? This removes all binding and group access entries.')) return
    setError(null)
    setResettingAcl(nodeId)
    fetch(`/api/resetacl/${nodeId}`, { method: 'POST' })
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`) })
      .catch((e: unknown) => setError(e instanceof Error ? e.message : 'Reset ACL failed'))
      .finally(() => setResettingAcl(null))
  }

  function handleViewAcl(nodeId: number) {
    setAclNodeId(nodeId)
    setAcl(null)
    setAclError(null)
    setAclLoading(true)
    fetch(`/api/acl/${nodeId}`)
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<AclResponse> })
      .then(data => setAcl(data.entries))
      .catch((e: unknown) => setAclError(e instanceof Error ? e.message : 'Failed to load ACL'))
      .finally(() => setAclLoading(false))
  }

  function handleViewBindings(nodeId: number) {
    setBindingsNodeId(nodeId)
    setBindings(null)
    setBindingsError(null)
    setBindingsLoading(true)
    fetch(`/api/bindingtable/${nodeId}`)
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<BindingTableResponse> })
      .then(data => setBindings(data.entries))
      .catch((e: unknown) => setBindingsError(e instanceof Error ? e.message : 'Failed to load bindings'))
      .finally(() => setBindingsLoading(false))
  }

  function handleViewGroupState(nodeId: number) {
    setGroupStateNodeId(nodeId)
    setGroupState(null)
    setGroupStateError(null)
    setGroupStateLoading(true)
    fetch(`/api/groupstate/${nodeId}`)
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<GroupStateResponse> })
      .then(data => setGroupState(data))
      .catch((e: unknown) => setGroupStateError(e instanceof Error ? e.message : 'Failed to load group state'))
      .finally(() => setGroupStateLoading(false))
  }

  function handleViewThreadInfo(nodeId: number) {
    setThreadInfoNodeId(nodeId)
    setThreadInfo(null)
    setThreadInfoError(null)
    setThreadInfoLoading(true)
    fetch(`/api/threadinfo/${nodeId}`)
      .then(r => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json() as Promise<ThreadInfoResponse> })
      .then(data => setThreadInfo(data))
      .catch((e: unknown) => setThreadInfoError(e instanceof Error ? e.message : 'Failed to load Thread info'))
      .finally(() => setThreadInfoLoading(false))
  }

  if (loading) return <p className="mt-3">Loading…</p>

  return (
    <>
      <div className="mt-3 mb-2 d-flex align-items-center justify-content-between">
        <h1>Devices</h1>
        <button className="btn btn-primary" onClick={() => navigate('/devices/pair')}>
          Pair Device
        </button>
      </div>
      <hr />
      {error && <div className="alert alert-danger">{error}</div>}
      {devices.length === 0 ? (
        <div className="alert alert-info">No devices have been commissioned yet.</div>
      ) : (
        <div className="d-flex flex-column gap-2">
          {devices.map(dev => {
            const nodeLabel = `Node 0x${dev.nodeId.toString(16).toUpperCase()}`
            const vendorProduct = [dev.vendorName, dev.productName].filter(Boolean).join(' ')
            const displayName = dev.name || vendorProduct || nodeLabel
            return (
              <div
                key={dev.nodeId}
                style={{ border: '1px solid #dee2e6', borderRadius: 6, overflow: 'hidden' }}
              >
                <div style={{ background: '#f8f9fa', borderBottom: '1px solid #dee2e6', padding: '8px 12px', display: 'flex', alignItems: 'center', gap: 8 }}>
                  <span style={{ fontSize: 20 }}>{deviceTypeIcon(dev.deviceType)}</span>
                  <div>
                    <strong
                      onClick={() => handleRename(dev)}
                      title="Click to rename"
                      style={{ cursor: 'pointer' }}
                    >
                      {displayName}
                    </strong>
                    {displayName !== nodeLabel && (
                      <span className="text-muted ms-2" style={{ fontSize: '0.8rem' }}>
                        {nodeLabel}
                      </span>
                    )}
                  </div>
                </div>
                <div style={{ padding: '8px 12px', display: 'flex', flexDirection: 'column', gap: 6 }}>
                  {(() => {
                    const typed = (dev.endpoints ?? []).filter(ep => ep.deviceTypes.length > 0)
                    if (typed.length === 0) {
                      return (
                        <span className="badge" style={{ background: '#1e40af', color: '#fff', padding: '3px 8px', borderRadius: 10, fontSize: '0.75rem' }}>
                          {deviceTypeName(dev.deviceType)}
                        </span>
                      )
                    }
                    return typed.map(ep => (
                      <div key={ep.endpointId} style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
                        <span className="text-muted" style={{ fontSize: '0.7rem', minWidth: 42 }}>
                          EP {ep.endpointId}
                        </span>
                        {ep.deviceTypes.map(dt => (
                          <span key={dt} className="badge" style={{ background: '#1e40af', color: '#fff', padding: '3px 8px', borderRadius: 10, fontSize: '0.75rem' }}>
                            {deviceTypeName(dt)}
                          </span>
                        ))}
                        {ep.label && (
                          <span className="text-muted" style={{ fontSize: '0.75rem' }}>{ep.label}</span>
                        )}
                      </div>
                    ))
                  })()}
                </div>
                <div style={{ padding: '8px 12px', borderTop: '1px solid #dee2e6' }} className="d-flex gap-2">
                  <button
                    className="btn btn-secondary btn-sm"
                    disabled={reinterviewing !== null}
                    onClick={() => handleReinterview(dev.nodeId)}
                  >
                    {reinterviewing === dev.nodeId ? 'Re-interviewing…' : 'Re-interview'}
                  </button>
                  <button
                    className="btn btn-secondary btn-sm"
                    disabled={reinterviewing !== null}
                    onClick={() => handleViewAcl(dev.nodeId)}
                  >
                    View ACL
                  </button>
                  <button
                    className="btn btn-secondary btn-sm"
                    disabled={reinterviewing !== null}
                    onClick={() => handleViewBindings(dev.nodeId)}
                  >
                    View Bindings
                  </button>
                  <button
                    className="btn btn-secondary btn-sm"
                    disabled={reinterviewing !== null}
                    onClick={() => handleViewGroupState(dev.nodeId)}
                  >
                    View Group State
                  </button>
                  <button
                    className="btn btn-secondary btn-sm"
                    disabled={reinterviewing !== null}
                    onClick={() => handleViewThreadInfo(dev.nodeId)}
                  >
                    View Thread Info
                  </button>
                  <button
                    className="btn btn-warning btn-sm"
                    disabled={reinterviewing !== null || resettingGroups !== null}
                    onClick={() => handleResetGroups(dev.nodeId)}
                  >
                    {resettingGroups === dev.nodeId ? 'Resetting Groups…' : 'Reset Groups'}
                  </button>
                  <button
                    className="btn btn-warning btn-sm"
                    disabled={reinterviewing !== null || resettingAcl !== null}
                    onClick={() => handleResetAcl(dev.nodeId)}
                  >
                    {resettingAcl === dev.nodeId ? 'Resetting ACL…' : 'Reset ACL'}
                  </button>
                  <button
                    className="btn btn-warning btn-sm"
                    disabled={reinterviewing !== null || resettingBindings !== null}
                    onClick={() => handleResetBindings(dev.nodeId)}
                  >
                    {resettingBindings === dev.nodeId ? 'Resetting Bindings…' : 'Reset Bindings'}
                  </button>
                  <button
                    className="btn btn-danger btn-sm"
                    disabled={reinterviewing !== null}
                    onClick={() => handleDelete(dev.nodeId)}
                  >
                    Remove
                  </button>
                </div>
              </div>
            )
          })}
        </div>
      )}
      {aclNodeId !== null && (
        <Modal
          title={`Access Control List · Node 0x${aclNodeId.toString(16).toUpperCase()}`}
          onClose={() => setAclNodeId(null)}
        >
          {aclLoading ? (
            <div className="d-flex align-items-center gap-2">
              <div className="spinner-border spinner-border-sm" role="status" />
              <span>Loading ACL…</span>
            </div>
          ) : aclError ? (
            <div className="alert alert-danger mb-0">{aclError}</div>
          ) : (() => {
            // Entries with privilege 0 are empty padding slots or other fabrics'
            // entries returned as zeroed placeholders by a fabric-filtered read —
            // this controller can neither read their detail nor remove them.
            const visible = (acl ?? []).filter(e => e.privilege !== 0)
            const hidden = (acl ?? []).length - visible.length
            if (visible.length === 0)
              return <div className="alert alert-info mb-0">This device has no ACL entries for this controller's fabric.</div>
            return (
              <div className="d-flex flex-column gap-2">
                {visible.map((entry, i) => (
                  <div key={i} style={{ border: '1px solid #dee2e6', borderRadius: 6, padding: '8px 12px' }}>
                    <div style={{ fontWeight: 600, marginBottom: 4 }}>Entry {i + 1}</div>
                    <div style={{ fontSize: '0.85rem' }}>
                      <div>Privilege: {PRIVILEGE_NAMES[entry.privilege] ?? 'Unknown'} ({entry.privilege})</div>
                      <div>Auth mode: {AUTH_MODE_NAMES[entry.authMode] ?? 'Unknown'} ({entry.authMode})</div>
                      <div>
                        Subjects: {entry.subjects.length > 0 ? entry.subjects.map(formatSubject).join(', ') : <span className="text-muted">none</span>}
                      </div>
                    </div>
                  </div>
                ))}
                {hidden > 0 && (
                  <div className="text-muted" style={{ fontSize: '0.8rem' }}>
                    {hidden} empty / other-fabric {hidden === 1 ? 'slot' : 'slots'} hidden (not owned by this controller).
                  </div>
                )}
              </div>
            )
          })()}
        </Modal>
      )}
      {bindingsNodeId !== null && (
        <Modal
          title={`Binding Table · Node 0x${bindingsNodeId.toString(16).toUpperCase()}`}
          onClose={() => setBindingsNodeId(null)}
        >
          {bindingsLoading ? (
            <div className="d-flex align-items-center gap-2">
              <div className="spinner-border spinner-border-sm" role="status" />
              <span>Loading bindings…</span>
            </div>
          ) : bindingsError ? (
            <div className="alert alert-danger mb-0">{bindingsError}</div>
          ) : bindings && bindings.length === 0 ? (
            <div className="alert alert-info mb-0">This device has no bindings.</div>
          ) : (
            <div className="d-flex flex-column gap-2">
              {(bindings ?? []).map((t, i) => (
                <div key={i} style={{ border: '1px solid #dee2e6', borderRadius: 6, padding: '8px 12px' }}>
                  <div style={{ fontWeight: 600, marginBottom: 4 }}>Binding {i + 1}</div>
                  <div style={{ fontSize: '0.85rem' }}>
                    {t.group !== undefined ? (
                      <div>Target group: {t.group}</div>
                    ) : (
                      <>
                        <div>Target node: {formatSubject(t.node ?? '0')}</div>
                        <div>Endpoint: {t.endpoint}</div>
                      </>
                    )}
                    <div>Cluster: {formatCluster(t.cluster)} ({t.cluster})</div>
                  </div>
                </div>
              ))}
            </div>
          )}
        </Modal>
      )}
      {groupStateNodeId !== null && (
        <Modal
          title={`Group State · Node 0x${groupStateNodeId.toString(16).toUpperCase()}`}
          onClose={() => setGroupStateNodeId(null)}
        >
          {groupStateLoading ? (
            <div className="d-flex align-items-center gap-2">
              <div className="spinner-border spinner-border-sm" role="status" />
              <span>Loading group state…</span>
            </div>
          ) : groupStateError ? (
            <div className="alert alert-danger mb-0">{groupStateError}</div>
          ) : (
            <div className="d-flex flex-column gap-3">
              <div>
                <div style={{ fontWeight: 600, marginBottom: 4 }}>GroupKeyMap (group → keyset)</div>
                {groupState && groupState.groupKeyMap.length > 0 ? (
                  <div className="d-flex flex-column gap-1">
                    {groupState.groupKeyMap.map((e, i) => (
                      <div key={i} style={{ border: '1px solid #dee2e6', borderRadius: 6, padding: '6px 10px', fontSize: '0.85rem' }}>
                        Group {e.group} → keyset {e.keyset}
                      </div>
                    ))}
                  </div>
                ) : (
                  <div className="alert alert-warning mb-0">No GroupKeyMap entries — this device can't derive any group key.</div>
                )}
              </div>
              <div>
                <div style={{ fontWeight: 600, marginBottom: 4 }}>GroupTable (group → member endpoints)</div>
                {groupState && groupState.groupTable.length > 0 ? (
                  <div className="d-flex flex-column gap-1">
                    {groupState.groupTable.map((e, i) => (
                      <div key={i} style={{ border: '1px solid #dee2e6', borderRadius: 6, padding: '6px 10px', fontSize: '0.85rem' }}>
                        Group {e.group}{e.name ? ` "${e.name}"` : ''} → endpoints {e.endpoints.length > 0 ? e.endpoints.join(', ') : <span className="text-muted">none</span>}
                      </div>
                    ))}
                  </div>
                ) : (
                  <div className="alert alert-warning mb-0">No GroupTable entries — no endpoint on this device belongs to any group.</div>
                )}
              </div>
            </div>
          )}
        </Modal>
      )}
      {threadInfoNodeId !== null && (
        <Modal
          title={`Thread Info · Node 0x${threadInfoNodeId.toString(16).toUpperCase()}`}
          onClose={() => setThreadInfoNodeId(null)}
        >
          {threadInfoLoading ? (
            <div className="d-flex align-items-center gap-2">
              <div className="spinner-border spinner-border-sm" role="status" />
              <span>Loading Thread info…</span>
            </div>
          ) : threadInfoError ? (
            <div className="alert alert-danger mb-0">{threadInfoError}</div>
          ) : threadInfo && threadInfo.extendedPanId === null && threadInfo.networkName === null ? (
            <div className="alert alert-info mb-0">This device reports no Thread diagnostics (it may not be on a Thread network).</div>
          ) : (
            <div className="d-flex flex-column gap-2" style={{ fontSize: '0.85rem' }}>
              <div className="text-muted" style={{ fontSize: '0.8rem' }}>
                Compare <strong>Extended PAN ID</strong> across group members — a different value means a different Thread network, so the group multicast can't reach it.
              </div>
              <div style={{ border: '1px solid #dee2e6', borderRadius: 6, padding: '8px 12px' }}>
                <div>Network name: {threadInfo?.networkName ?? <span className="text-muted">n/a</span>}</div>
                <div>Extended PAN ID: {threadInfo?.extendedPanId ?? <span className="text-muted">n/a</span>}</div>
                <div>PAN ID: {threadInfo?.panId != null ? `0x${threadInfo.panId.toString(16).toUpperCase()}` : <span className="text-muted">n/a</span>}</div>
                <div>Channel: {threadInfo?.channel ?? <span className="text-muted">n/a</span>}</div>
                <div>Routing role: {threadInfo?.routingRole != null ? `${ROUTING_ROLE_NAMES[threadInfo.routingRole] ?? 'Unknown'} (${threadInfo.routingRole})` : <span className="text-muted">n/a</span>}</div>
              </div>
            </div>
          )}
        </Modal>
      )}
    </>
  )
}

export default Devices
