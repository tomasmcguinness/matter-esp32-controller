import {
  ReactFlow,
  ReactFlowProvider,
  Background,
  BackgroundVariant,
  Panel,
  NodeResizer,
  useNodesState,
  useEdgesState,
  useReactFlow,
  type Node,
  type Edge,
  type EdgeChange,
  type NodeChange,
  addEdge,
  Handle,
  Position,
} from '@xyflow/react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { deviceTypeName, deviceTypeIcon, isOnOffDevice, isSwitchDevice } from './deviceTypeName'
import type { CommissionedDevice, DeviceEndpoint } from './Devices'

type DeviceNodeData = {
  label: string
  nodeId: number
  deviceType: number
  endpoints?: DeviceEndpoint[]
  // Set when this device is a member of a group sub-flow (runtime convenience;
  // membership is persisted via the node's parentId in settings).
  groupId?: number
}

function OnOffSwitch({ nodeId }: { nodeId: number }) {
  // null = loading/unknown, true/false = known state
  const [on, setOn] = useState<boolean | null>(null)
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    let cancelled = false
    fetch(`/api/onoff/${nodeId}`)
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((d: { on: boolean }) => { if (!cancelled) setOn(!!d.on) })
      .catch(() => { if (!cancelled) setOn(null) })
    return () => { cancelled = true }
  }, [nodeId])

  const toggle = useCallback((e: React.MouseEvent) => {
    e.stopPropagation()
    if (busy) return
    const next = !(on ?? false)
    const prev = on
    setOn(next)
    setBusy(true)
    fetch(`/api/onoff/${nodeId}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ on: next }),
    })
      .then(r => { if (!r.ok) throw new Error() })
      .catch(() => setOn(prev))
      .finally(() => setBusy(false))
  }, [nodeId, on, busy])

  const known = on !== null
  return (
    <button
      className="nodrag"
      onClick={toggle}
      title={on ? 'On' : 'Off'}
      style={{
        marginTop: 6,
        width: 40,
        height: 22,
        borderRadius: 11,
        border: 'none',
        padding: 0,
        cursor: 'pointer',
        position: 'relative',
        background: on ? '#16a34a' : '#cbd5e1',
        opacity: known ? 1 : 0.5,
        transition: 'background 0.15s',
      }}
    >
      <span style={{
        position: 'absolute',
        top: 2,
        left: on ? 20 : 2,
        width: 18,
        height: 18,
        borderRadius: '50%',
        background: '#fff',
        transition: 'left 0.15s',
      }} />
    </button>
  )
}

function DeviceNode({ data }: { data: DeviceNodeData }) {
  const icon = deviceTypeIcon(data.deviceType)
  const typeName = deviceTypeName(data.deviceType)
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null)

  useEffect(() => {
    if (!menu) return
    const close = () => setMenu(null)
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setMenu(null) }
    window.addEventListener('click', close)
    window.addEventListener('keydown', onKey)
    return () => {
      window.removeEventListener('click', close)
      window.removeEventListener('keydown', onKey)
    }
  }, [menu])

  const openMenu = useCallback((e: React.MouseEvent) => {
    e.preventDefault()
    e.stopPropagation()
    setMenu({ x: e.clientX, y: e.clientY })
  }, [])

  const identify = useCallback((e: React.MouseEvent) => {
    e.stopPropagation()
    setMenu(null)
    fetch(`/api/identify/${data.nodeId}`, { method: 'POST' }).catch(() => { })
  }, [data.nodeId])

  // Sorted endpoint breakdown; empty for nodes not yet interrogated (legacy view).
  const eps = (data.endpoints ?? []).slice().sort((a, b) => a.endpointId - b.endpointId)
  const hasEndpoints = eps.length > 0

  return (
    <>
      {/* Legacy node-level handles only when we have no per-endpoint breakdown. */}
      {!hasEndpoints && <Handle type="target" position={Position.Left} id="binding-target" />}
      {!hasEndpoints && <Handle type="source" position={Position.Right} id="binding-source" />}
      <div style={{ minWidth: 170 }} onContextMenu={openMenu}>
        <div style={{
          background: '#1e40af',
          color: '#fff',
          padding: '4px 10px',
          fontSize: 11,
          fontWeight: 600,
          display: 'flex',
          alignItems: 'center',
          gap: 6,
          whiteSpace: 'nowrap',
        }}>
          <span>{icon}</span>
          <span style={{ flex: 1 }}>{data.label}</span>
        </div>
        <div style={{ padding: '5px 10px', fontSize: 12 }}>
          <div style={{ color: '#64748b', fontSize: 11 }}>
            Node 0x{data.nodeId.toString(16).toUpperCase()}
          </div>
          {hasEndpoints ? (
            <div style={{ marginTop: 4, display: 'flex', flexDirection: 'column', gap: 4 }}>
              {eps.map(ep => {
                const isLight = ep.deviceTypes.some(isOnOffDevice)
                const isSwitch = ep.deviceTypes.some(isSwitchDevice)
                const typeLabel = ep.deviceTypes.map(dt => deviceTypeName(dt)).join(', ') || '—'
                return (
                  <div key={ep.endpointId} style={{ position: 'relative', display: 'flex', alignItems: 'center', gap: 6, minHeight: 22 }}>
                    {/* On/Off endpoint is a binding target (light); switch endpoint is a binding source. */}
                    {isLight && <Handle type="target" position={Position.Left} id={`binding-target-${ep.endpointId}`} style={{ left: -10 }} />}
                    <span style={{ color: '#94a3b8', fontSize: 10, minWidth: 30 }}>EP {ep.endpointId}</span>
                    <span style={{ color: '#475569', flex: 1 }}>
                      {typeLabel}{ep.label ? ` (${ep.label})` : ''}
                    </span>
                    {/* Node-level OnOff API resolves the first On/Off endpoint; multiple On/Off
                        endpoints on one node are not disambiguated (rare; out of scope). */}
                    {isLight && <OnOffSwitch nodeId={data.nodeId} />}
                    {isSwitch && <Handle type="source" position={Position.Right} id={`binding-source-${ep.endpointId}`} style={{ right: -10 }} />}
                  </div>
                )
              })}
            </div>
          ) : (
            <>
              <div style={{ color: '#475569', marginTop: 2 }}>{typeName}</div>
              {isOnOffDevice(data.deviceType) && <OnOffSwitch nodeId={data.nodeId} />}
            </>
          )}
        </div>
      </div>
      {menu && (
        <div
          className="nodrag"
          onClick={e => e.stopPropagation()}
          style={{
            position: 'fixed',
            top: menu.y,
            left: menu.x,
            zIndex: 1000,
            background: '#fff',
            border: '1px solid #cbd5e1',
            borderRadius: 4,
            boxShadow: '0 2px 8px rgba(0,0,0,0.15)',
            padding: '4px 0',
            fontSize: 12,
            minWidth: 120,
          }}
        >
          <button
            onClick={identify}
            style={{
              display: 'block',
              width: '100%',
              textAlign: 'left',
              background: 'none',
              border: 'none',
              padding: '6px 12px',
              cursor: 'pointer',
              fontSize: 12,
            }}
          >
            Identify
          </button>
        </div>
      )}
    </>
  )
}

type GroupNodeData = {
  label: string
  groupId: number
  // Cosmetic only — all groups share one keyset on the firmware; kept for display/persistence.
  keysetId?: number
}

// A Matter group rendered as a ReactFlow sub-flow: a resizable container whose
// children (dropped device nodes) are its members. The left target handle lets a
// switch be wired to the whole group (group binding).
function GroupNode({ data, selected }: { data: GroupNodeData; selected?: boolean }) {
  // Controller-originated OnOff Toggle groupcast — drives the group directly so you
  // can verify it works independently of any bound switch.
  const groupcastToggle = useCallback((e: React.MouseEvent) => {
    e.stopPropagation()
    fetch(`/api/grouptoggle/${data.groupId}`, { method: 'POST' }).catch(() => { })
  }, [data.groupId])

  const btnStyle: React.CSSProperties = {
    border: '1px solid #c7d2fe', borderRadius: 4, background: '#eef2ff',
    color: '#4f46e5', fontSize: 10, fontWeight: 600, padding: '1px 7px', cursor: 'pointer',
  }

  return (
    <>
      <NodeResizer minWidth={180} minHeight={120} isVisible={!!selected} />
      <Handle type="target" position={Position.Left} id="group-target" />
      <div style={{
        width: '100%',
        height: '100%',
        border: '2px dashed #6366f1',
        borderRadius: 8,
        background: 'rgba(99,102,241,0.06)',
        boxSizing: 'border-box',
        marginTop: '10px'
      }}>
        <div style={{
          position: 'absolute',
          top: 4,
          left: 8,
          right: 8,
          display: 'flex',
          alignItems: 'center',
          gap: 6,
        }}>
          <span style={{ fontSize: 11, fontWeight: 700, color: '#4f46e5', flex: 1, pointerEvents: 'none' }}>
            👥 {data.label}
          </span>
          <button className="nodrag" style={btnStyle} title="Groupcast Toggle" onClick={groupcastToggle}>Toggle</button>
        </div>
      </div>
    </>
  )
}

const nodeTypes = { device: DeviceNode, group: GroupNode }

type SavedNodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
type SavedEdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

// Extracts the endpoint id from a per-endpoint binding handle id, e.g.
// "binding-source-2" -> 2. Returns null for legacy node-level handles.
function parseEndpointHandle(handle: string | null | undefined): number | null {
  if (!handle) return null
  const m = handle.match(/^binding-(?:source|target)-(\d+)$/)
  return m ? parseInt(m[1], 10) : null
}

// React Flow requires every parent node to appear before its children in the
// array. Group nodes are never nested, so listing them first guarantees this.
function orderGroupsFirst(nodes: Node[]): Node[] {
  const groups = nodes.filter(n => n.type === 'group')
  const rest = nodes.filter(n => n.type !== 'group')
  return [...groups, ...rest]
}

// Full settings blob for a device node (the firmware replaces settings wholesale
// on PUT, so we always send the complete object). parentId encodes group membership.
function deviceNodeSettings(node: Node): Record<string, unknown> {
  const d = node.data as DeviceNodeData
  const s: Record<string, unknown> = { label: d.label, nodeId: d.nodeId, deviceType: d.deviceType }
  if (node.parentId) s.parentId = node.parentId
  return s
}

const JSON_HEADERS = { 'Content-Type': 'application/json' }

function Canvas() {
  const [nodes, setNodes, onNodesChangeBase] = useNodesState<Node>([])
  const [edges, setEdges, onEdgesChangeBase] = useEdgesState<Edge>([])
  const nodeIdCounter = useRef(10)
  const { getIntersectingNodes } = useReactFlow()

  // Always-current refs so edge/connect callbacks can resolve node device types.
  const nodesRef = useRef<Node[]>([])
  const edgesRef = useRef<Edge[]>([])
  nodesRef.current = nodes
  edgesRef.current = edges

  // Returns the Matter binding body if the edge connects a switch (source) to a
  // bindable light (target), otherwise null (the edge stays purely visual).
  const bindingBodyFor = useCallback((
    sourceId: string, sourceHandle: string | null | undefined,
    targetId: string, targetHandle: string | null | undefined,
  ) => {
    const src = nodesRef.current.find(n => n.id === sourceId)
    const tgt = nodesRef.current.find(n => n.id === targetId)
    if (!src?.data) return null

    // Switch -> group sub-flow: a Matter group binding (firmware branches on groupId).
    if (tgt?.type === 'group') {
      // Accept the source either via a per-endpoint switch source handle (only
      // rendered for switch endpoints, e.g. "binding-source-2") or, for legacy
      // node-level handles, the node's primary device type. This is what lets a
      // switch endpoint on a combined light+switch device bind to the group.
      const switchEndpoint = parseEndpointHandle(sourceHandle)
      const srcType = src.data.deviceType as number | undefined
      const isSwitchSource = switchEndpoint !== null || (srcType !== undefined && isSwitchDevice(srcType))
      if (!isSwitchSource) return null
      const body: Record<string, unknown> = {
        switchNodeId: src.data.nodeId as number,
        groupId: (tgt.data as GroupNodeData).groupId,
      }
      if (switchEndpoint !== null) body.switchEndpoint = switchEndpoint
      return body
    }

    if (!tgt?.data) return null
    const switchNodeId = src.data.nodeId as number
    const lightNodeId = tgt.data.nodeId as number

    // Per-endpoint handles carry the endpoint id (e.g. "binding-source-2"); when
    // both ends do, bind those specific endpoints.
    const switchEndpoint = parseEndpointHandle(sourceHandle)
    const lightEndpoint = parseEndpointHandle(targetHandle)
    if (switchEndpoint !== null && lightEndpoint !== null) {
      return { switchNodeId, switchEndpoint, lightNodeId, lightEndpoint }
    }

    // Legacy node-level handles: validate via primary device type and let the
    // firmware auto-resolve the endpoints.
    const srcType = src.data.deviceType as number | undefined
    const tgtType = tgt.data.deviceType as number | undefined
    if (srcType === undefined || tgtType === undefined) return null
    if (!isSwitchDevice(srcType) || !isOnOffDevice(tgtType)) return null
    return { switchNodeId, lightNodeId }
  }, [])

  const deleteBinding = useCallback((
    sourceId: string, sourceHandle: string | null | undefined,
    targetId: string, targetHandle: string | null | undefined,
  ) => {
    const body = bindingBodyFor(sourceId, sourceHandle, targetId, targetHandle)
    if (!body) return
    fetch('/api/bindings', {
      method: 'DELETE',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    }).catch(() => { })
  }, [bindingBodyFor])

  // Node deletion is handled natively by React Flow (deleteKeyCode). React Flow respects
  // node.deletable === false, so non-deletable nodes are skipped automatically. We mirror
  // the removal to the firmware's node store, and tear down the Matter group (or drop the
  // member) for group/member nodes.
  const onNodesDelete = useCallback((deleted: Node[]) => {
    deleted.forEach(n => {
      fetch(`/api/nodes/${n.id}`, { method: 'DELETE' }).catch(() => { })
      if (n.type === 'group') {
        const g = n.data as GroupNodeData
        fetch(`/api/groups/${g.groupId}`, { method: 'DELETE' }).catch(() => { })
      } else if (n.parentId) {
        const d = n.data as DeviceNodeData
        if (d.groupId !== undefined)
          fetch(`/api/groups/${d.groupId}/members/${d.nodeId}`, { method: 'DELETE' }).catch(() => { })
      }
    })
  }, [])

  // Persist a node's position only (firmware preserves existing settings when none sent).
  const persistPosition = useCallback((id: string, x: number, y: number) => {
    fetch(`/api/nodes/${id}`, {
      method: 'PUT', headers: JSON_HEADERS, body: JSON.stringify({ x, y }),
    }).catch(() => { })
  }, [])

  // Add a device to a group on the firmware, and (when moving between groups) drop the old one.
  const joinGroup = useCallback((nodeId: number, label: string, groupId: number, prevGroupId?: number) => {
    fetch(`/api/groups/${groupId}/members`, {
      method: 'POST', headers: JSON_HEADERS, body: JSON.stringify({ nodeId, name: label }),
    }).catch(() => { })
    if (prevGroupId !== undefined && prevGroupId !== groupId)
      fetch(`/api/groups/${prevGroupId}/members/${nodeId}`, { method: 'DELETE' }).catch(() => { })
  }, [])

  const onNodeDragStop = useCallback((_: React.MouseEvent, node: Node) => {
    if (node.type === 'group') {
      persistPosition(node.id, node.position.x, node.position.y)
      return
    }

    const d = node.data as DeviceNodeData
    // node.position is relative to the parent when the device currently sits in a group.
    const prevParent = node.parentId ? nodesRef.current.find(n => n.id === node.parentId) : undefined
    const abs = prevParent
      ? { x: prevParent.position.x + node.position.x, y: prevParent.position.y + node.position.y }
      : { x: node.position.x, y: node.position.y }
    const prevGroupId = prevParent ? (prevParent.data as GroupNodeData).groupId : undefined

    const groupHit = getIntersectingNodes(node).find(n => n.type === 'group')

    // Joining (or moving between) groups: reparent + persist relative position + membership.
    if (groupHit && groupHit.id !== node.parentId) {
      const groupId = (groupHit.data as GroupNodeData).groupId
      const rel = { x: abs.x - groupHit.position.x, y: abs.y - groupHit.position.y }
      // No extent:'parent' — that would hard-clamp the device inside the box and
      // make it impossible to drag back out. Membership is re-evaluated on drop.
      const updated: Node = {
        ...node, parentId: groupHit.id, extent: undefined, position: rel,
        data: { ...d, groupId },
      }
      setNodes(nds => orderGroupsFirst(nds.map(n => (n.id === node.id ? updated : n))))
      fetch(`/api/nodes/${node.id}`, {
        method: 'PUT', headers: JSON_HEADERS,
        body: JSON.stringify({ x: rel.x, y: rel.y, settings: deviceNodeSettings(updated) }),
      }).catch(() => { })
      joinGroup(d.nodeId, d.label, groupId, prevGroupId)
      return
    }

    // Leaving a group (dropped outside every group): unparent + restore absolute position.
    if (!groupHit && prevParent) {
      const updated: Node = {
        ...node, parentId: undefined, extent: undefined, position: abs,
        data: { ...d, groupId: undefined },
      }
      setNodes(nds => nds.map(n => (n.id === node.id ? updated : n)))
      fetch(`/api/nodes/${node.id}`, {
        method: 'PUT', headers: JSON_HEADERS,
        body: JSON.stringify({ x: abs.x, y: abs.y, settings: deviceNodeSettings(updated) }),
      }).catch(() => { })
      fetch(`/api/groups/${prevGroupId}/members/${d.nodeId}`, { method: 'DELETE' }).catch(() => { })
      return
    }

    // No membership change: persist the (possibly relative) position.
    persistPosition(node.id, node.position.x, node.position.y)
  }, [getIntersectingNodes, setNodes, persistPosition, joinGroup])

  // Persist a group's new size after a NodeResizer drag completes.
  const onNodesChange = useCallback((changes: NodeChange[]) => {
    onNodesChangeBase(changes)
    for (const c of changes) {
      if (c.type !== 'dimensions' || c.resizing === true) continue
      const n = nodesRef.current.find(x => x.id === c.id)
      if (n?.type !== 'group') continue
      const g = n.data as GroupNodeData
      fetch(`/api/nodes/${n.id}`, {
        method: 'PUT', headers: JSON_HEADERS,
        body: JSON.stringify({
          x: n.position.x, y: n.position.y,
          settings: { type: 'group', groupId: g.groupId, keysetId: g.keysetId, label: g.label,
            width: c.dimensions?.width, height: c.dimensions?.height },
        }),
      }).catch(() => { })
    }
  }, [onNodesChangeBase])

  // Create a group: allocate ids on the firmware, then drop a sub-flow container.
  const addGroup = useCallback(async () => {
    try {
      const res = await fetch('/api/groups', { method: 'POST', headers: JSON_HEADERS, body: '{}' })
      if (!res.ok) return
      const { groupId } = await res.json() as { groupId: number }
      const id = `group_${groupId}`
      const label = `Group ${groupId}`
      const width = 280, height = 200
      const position = { x: 80, y: 80 }
      const node: Node = {
        id, type: 'group', position, style: { width, height },
        data: { label, groupId } as GroupNodeData,
      }
      setNodes(nds => orderGroupsFirst([...nds, node]))
      fetch(`/api/nodes/${id}`, {
        method: 'PUT', headers: JSON_HEADERS,
        body: JSON.stringify({ x: position.x, y: position.y,
          settings: { type: 'group', groupId, label, width, height } }),
      }).catch(() => { })
    } catch { /* ignore */ }
  }, [setNodes])

  const onEdgesChange = useCallback((changes: EdgeChange[]) => {
    changes.filter(c => c.type === 'remove').forEach(c => {
      const id = (c as { id: string }).id
      fetch(`/api/edges/${id}`, { method: 'DELETE' }).catch(() => { })
      const edge = edgesRef.current.find(e => e.id === id)
      if (edge) deleteBinding(edge.source, edge.sourceHandle, edge.target, edge.targetHandle)
    })
    onEdgesChangeBase(changes)
  }, [onEdgesChangeBase, deleteBinding])

  const onConnect = useCallback((params: { source: string; sourceHandle?: string | null; target: string; targetHandle?: string | null }) => {
    const edgeId = [params.source, params.sourceHandle, params.target, params.targetHandle].filter(Boolean).join('-')
    const newEdge: Edge = { ...params, id: edgeId }
    setEdges(eds => addEdge(newEdge, eds))
    fetch('/api/edges', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        id: edgeId,
        source: params.source,
        target: params.target,
        sourceHandle: params.sourceHandle ?? null,
        targetHandle: params.targetHandle ?? null,
      }),
    }).catch(() => { })

    // If a switch was connected to a light, create the Matter binding too.
    const binding = bindingBodyFor(params.source, params.sourceHandle, params.target, params.targetHandle)
    if (binding) {
      fetch('/api/bindings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(binding),
      }).catch(() => { })
    }
  }, [setEdges, bindingBodyFor])

  const onInit = useCallback(() => {
    Promise.all([
      fetch('/api/nodes').then(r => r.ok ? r.json() : Promise.reject()) as Promise<{ nodes: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }>,
      fetch('/api/devices').then(r => r.ok ? r.json() : { devices: [] }).catch(() => ({ devices: [] })) as Promise<{ devices: CommissionedDevice[] }>,
    ])
      .then(([data, deviceData]) => {
        // Map nodeId -> endpoint breakdown so each canvas node can render its endpoints,
        // and nodeId -> custom device name (set on the Devices tab) for the node label.
        const epByNode = new Map<number, DeviceEndpoint[]>()
        const nameByNode = new Map<number, string>()
        for (const d of deviceData.devices ?? []) {
          if (d.endpoints) epByNode.set(d.nodeId, d.endpoints)
          if (d.name) nameByNode.set(d.nodeId, d.name)
        }

        // First pass: group sub-flows (parents must precede their children).
        const groupData = new Map<string, GroupNodeData>()
        const groupNodes: Node[] = []
        for (const n of data.nodes) {
          if (n.settings?.type !== 'group') continue
          const groupId = n.settings.groupId as number
          const gdata: GroupNodeData = {
            label: (n.settings.label as string) ?? `Group ${groupId}`,
            groupId,
            keysetId: (n.settings.keysetId as number) ?? groupId,
          }
          groupData.set(n.id, gdata)
          groupNodes.push({
            id: n.id,
            type: 'group',
            position: { x: n.x, y: n.y },
            style: { width: (n.settings.width as number) ?? 280, height: (n.settings.height as number) ?? 200 },
            data: gdata,
          })
        }

        // Second pass: device nodes, re-attached to their group via parentId.
        const deviceNodes: Node[] = []
        for (const n of data.nodes) {
          if (n.settings?.type === 'group') continue
          const nodeId = n.settings?.nodeId as number
          const parentId = n.settings?.parentId as string | undefined
          const parentGroup = parentId ? groupData.get(parentId) : undefined
          const node: Node = {
            id: n.id,
            type: 'device',
            position: { x: n.x, y: n.y },
            draggable: true,
            data: {
              // Prefer the custom device name from /api/devices, then any saved
              // canvas label, then the node id.
              label: nameByNode.get(nodeId) ?? (n.settings?.label as string) ?? n.id,
              nodeId,
              deviceType: n.settings?.deviceType as number ?? 0,
              endpoints: epByNode.get(nodeId),
              groupId: parentGroup?.groupId,
            },
          }
          // parentId only (no extent:'parent') so the device can be dragged out again.
          if (parentId && parentGroup) {
            node.parentId = parentId
          }
          deviceNodes.push(node)
        }

        for (const n of deviceNodes) {
          const m = n.id.match(/^node_(\d+)$/)
          if (m) nodeIdCounter.current = Math.max(nodeIdCounter.current, parseInt(m[1]))
        }

        setNodes([...groupNodes, ...deviceNodes])

        if (data.edges?.length) {
          setEdges(data.edges.map(e => ({
            id: e.id,
            source: e.source,
            target: e.target,
            sourceHandle: e.sourceHandle,
            targetHandle: e.targetHandle,
          })))
        }
      })
      .catch(() => console.log('Failed to load nodes from API'))
  }, [setNodes, setEdges])

  return (
    <div style={{ position: 'relative', height: 'calc(100vh - 60px)' }}>
      <ReactFlow
        style={{ height: '100%' }}
        nodes={nodes}
        onNodesChange={onNodesChange}
        onNodesDelete={onNodesDelete}
        edges={edges}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        nodeTypes={nodeTypes}
        onInit={onInit}
        onNodeDragStop={onNodeDragStop}
        deleteKeyCode={['Delete', 'Backspace']}
        nodesDraggable={true}
        nodesConnectable={true}
        defaultViewport={{ x: 0, y: 0, zoom: 1 }}
        nodeOrigin={[0, 0]}
      >
        <Panel position="top-left">
          <button
            onClick={addGroup}
            style={{
              background: '#4f46e5',
              color: '#fff',
              border: 'none',
              borderRadius: 6,
              padding: '6px 12px',
              fontSize: 12,
              fontWeight: 600,
              cursor: 'pointer',
              boxShadow: '0 1px 3px rgba(0,0,0,0.2)',
            }}
          >
            + Add Group
          </button>
        </Panel>
        <Background variant={BackgroundVariant.Lines} color="#cbd5e1" gap={24} size={1.5} />
      </ReactFlow>
    </div>
  )
}

export default () => (
  <ReactFlowProvider>
    <Canvas />
  </ReactFlowProvider>
)
