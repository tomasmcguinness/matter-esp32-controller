import {
  ReactFlow,
  ReactFlowProvider,
  Background,
  BackgroundVariant,
  useNodesState,
  useEdgesState,
  type Node,
  type Edge,
  type EdgeChange,
  addEdge,
  Handle,
  Position,
} from '@xyflow/react'
import { useCallback, useEffect, useRef } from 'react'
import { deviceTypeName, deviceTypeIcon } from './deviceTypeName'

type DeviceNodeData = {
  label: string
  nodeId: number
  deviceType: number
}

function DeviceNode({ data }: { data: DeviceNodeData }) {
  const icon = deviceTypeIcon(data.deviceType)
  const typeName = deviceTypeName(data.deviceType)
  return (
    <>
      <Handle type="target" position={Position.Left} id="binding-target" />
      <div style={{ minWidth: 140 }}>
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
          <div style={{ color: '#475569', marginTop: 2 }}>{typeName}</div>
        </div>
      </div>
      <Handle type="source" position={Position.Right} id="binding-source" />
    </>
  )
}

const nodeTypes = { device: DeviceNode }

type SavedNodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
type SavedEdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }


function Canvas() {
  const [nodes, setNodes, onNodesChange] = useNodesState<Node>([])
  const [edges, setEdges, onEdgesChangeBase] = useEdgesState<Edge>([])
  const nodeIdCounter = useRef(10)

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Delete' && e.key !== 'Backspace') return
      setNodes(nds => {
        const deletedIds = new Set(
          nds.filter(n => n.selected && n.deletable !== false).map(n => n.id)
        )
        if (deletedIds.size === 0) return nds
        setEdges(eds => {
          const removed = eds.filter(e => deletedIds.has(e.source) || deletedIds.has(e.target))
          removed.forEach(e => fetch(`/api/edges/${e.id}`, { method: 'DELETE' }).catch(() => { }))
          return eds.filter(e => !deletedIds.has(e.source) && !deletedIds.has(e.target))
        })
        for (const id of deletedIds) {
          fetch(`/api/nodes/${id}`, { method: 'DELETE' }).catch(() => { })
        }
        return nds.filter(n => !deletedIds.has(n.id))
      })
    }
    window.addEventListener('keydown', handleKeyDown)
    return () => window.removeEventListener('keydown', handleKeyDown)
  }, [setNodes, setEdges])

  const onNodeDragStop = useCallback((_: React.MouseEvent, node: Node) => {
    fetch(`/api/nodes/${node.id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: node.position.x, y: node.position.y }),
    }).catch(() => { })
  }, [])

  const onEdgesChange = useCallback((changes: EdgeChange[]) => {
    changes.filter(c => c.type === 'remove').forEach(c => {
      fetch(`/api/edges/${(c as { id: string }).id}`, { method: 'DELETE' }).catch(() => { })
    })
    onEdgesChangeBase(changes)
  }, [onEdgesChangeBase])

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
  }, [setEdges])

  const onInit = useCallback(() => {
    fetch('/api/nodes')
      .then(r => r.ok ? r.json() : Promise.reject())
      .then((data: { nodes: SavedNodeConfig[]; edges?: SavedEdgeConfig[] }) => {
        const restoredNodes: Node[] = data.nodes.map(n => ({
          id: n.id,
          type: 'device',
          position: { x: n.x, y: n.y },
          draggable: true,
          data: {
            label: (n.settings?.label as string) ?? n.id,
            nodeId: n.settings?.nodeId as number,
            deviceType: n.settings?.deviceType as number ?? 0,
          },
        }))

        for (const n of restoredNodes) {
          const m = n.id.match(/^node_(\d+)$/)
          if (m) nodeIdCounter.current = Math.max(nodeIdCounter.current, parseInt(m[1]))
        }

        setNodes(restoredNodes)

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
        edges={edges}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        nodeTypes={nodeTypes}
        onInit={onInit}
        onNodeDragStop={onNodeDragStop}
        nodesDraggable={true}
        nodesConnectable={true}
        defaultViewport={{ x: 0, y: 0, zoom: 1 }}
        nodeOrigin={[0, 0]}
      >
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
