import { http, HttpResponse, ws } from 'msw'
import type { CommissionedDevice } from '../Devices'
import type { ThreadCredentials, BorderAgent } from '../Thread'

type NodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
type EdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }
type BindingConfig = { switchNodeId: number; lightNodeId?: number; groupId?: number; switchEndpoint?: number; lightEndpoint?: number }

let nodeConfigs: NodeConfig[] = []
let edgeConfigs: EdgeConfig[] = []
let bindings: BindingConfig[] = []

// Group membership: groupId -> set of member node ids (mock-only bookkeeping).
const groupMembers: Record<number, Set<number>> = {}
let nextGroupId = 1

let devices: CommissionedDevice[] = [
  {
    nodeId: 0x1001,
    vendorName: 'Philips',
    productName: 'Hue White',
    deviceType: 0x0100,
    endpoints: [
      { endpointId: 0, label: '', included: true, deviceTypes: [0x0016], parts: [1, 2] },
      { endpointId: 1, label: 'Light', included: true, deviceTypes: [0x0100], parts: [] },
      { endpointId: 2, label: 'Switch', included: true, deviceTypes: [0x0103], parts: [] },
    ],
  },
  {
    nodeId: 0x1002,
    vendorName: 'IKEA',
    productName: 'TRADFRI bulb',
    deviceType: 0x0100,
    endpoints: [
      { endpointId: 0, label: '', included: true, deviceTypes: [0x0016], parts: [1] },
      { endpointId: 1, label: 'Bulb', included: true, deviceTypes: [0x0100], parts: [] },
    ],
  },
  {
    nodeId: 0x2001,
    vendorName: 'Eve',
    productName: 'Energy Plug',
    deviceType: 0x010a,
  },
  {
    nodeId: 0x2002,
    vendorName: 'Nanoleaf',
    productName: 'Essentials Bulb',
    deviceType: 0x010d,
  },
  {
    nodeId: 0x2003,
    vendorName: 'Aqara',
    productName: 'Wireless Switch',
    deviceType: 0x0103,
  },
]

// Seed the canvas with the mock devices so On/Off switches are visible in dev.
nodeConfigs = devices.map((d, i) => ({
  id: String(d.nodeId),
  x: 80 + 200 * (i % 4),
  y: 80 + 160 * Math.floor(i / 4),
  settings: { label: `Node 0x${d.nodeId.toString(16).toUpperCase()}`, nodeId: d.nodeId, deviceType: d.deviceType },
}))

// Mock OnOff state per node id.
const onoffState: Record<number, boolean> = {}

let threadCredentials: ThreadCredentials = { hasCredentials: false }

const borderAgents: BorderAgent[] = [
  { host: 'otbr-living-room.local', ip: '192.168.1.50', port: 49191, networkName: 'MyHome-Thread' },
  { host: 'otbr-office.local', ip: '192.168.1.51', port: 49152, networkName: 'Office-Thread' },
]

const controllerWs = ws.link('ws://*/ws')

let nextMockNodeId = 0x4001

export const handlers = [
  http.post('/controller/commission', async ({ request }) => {
    const body = (await request.json()) as { onboardingPayload: string }
    if (!body?.onboardingPayload) {
      return new HttpResponse('Missing onboardingPayload', { status: 400 })
    }
    const nodeId = nextMockNodeId++
    devices.push({ nodeId, vendorName: 'Acme', productName: 'Smart Plug', deviceType: 0x010a })
    nodeConfigs.push({
      id: String(nodeId),
      x: 80 + 200 * (nodeConfigs.length % 4),
      y: 80 + 160 * Math.floor(nodeConfigs.length / 4),
      settings: { label: `Node 0x${nodeId.toString(16).toUpperCase()}`, nodeId, deviceType: 0x010a },
    })
    return HttpResponse.json({ nodeId })
  }),

  http.get('/api/devices', () => {
    return HttpResponse.json({ devices })
  }),

  http.delete('/api/devices/:nodeId', ({ params }) => {
    const nodeId = Number(params.nodeId)
    devices = devices.filter(d => d.nodeId !== nodeId)
    return HttpResponse.json({})
  }),

  http.put('/api/devices/:nodeId', async ({ params, request }) => {
    const nodeId = Number(params.nodeId)
    const body = (await request.json()) as { name?: string }
    devices = devices.map(d => d.nodeId === nodeId ? { ...d, name: body.name ?? '' } : d)
    return HttpResponse.json({})
  }),

  http.post('/api/reinterview/:nodeId', async ({ params }) => {
    const nodeId = Number(params.nodeId)
    // Simulate the device being re-read; just confirm it still exists.
    if (!devices.some(d => d.nodeId === nodeId)) {
      return new HttpResponse('Not found', { status: 404 })
    }
    await new Promise(resolve => setTimeout(resolve, 600))
    return HttpResponse.json({})
  }),

  http.get('/api/onoff/:nodeId', ({ params }) => {
    const nodeId = Number(params.nodeId)
    return HttpResponse.json({ on: onoffState[nodeId] ?? false })
  }),

  http.put('/api/onoff/:nodeId', async ({ params, request }) => {
    const nodeId = Number(params.nodeId)
    const body = (await request.json()) as { on: boolean }
    onoffState[nodeId] = !!body.on
    return HttpResponse.json({})
  }),

  http.get('/api/acl/:nodeId', async ({ params }) => {
    const nodeId = Number(params.nodeId)
    if (!devices.some(d => d.nodeId === nodeId)) {
      return new HttpResponse('Not found', { status: 404 })
    }
    // Delay so the modal's loading indicator is visible during dev.
    await new Promise(resolve => setTimeout(resolve, 400))
    return HttpResponse.json({
      entries: [
        { privilege: 5, authMode: 2, subjects: ['112233'] },
        { privilege: 3, authMode: 2, subjects: [String(0x1000 + (nodeId % 16))] },
      ],
    })
  }),

  http.get('/api/bindingtable/:nodeId', async ({ params }) => {
    const nodeId = Number(params.nodeId)
    if (!devices.some(d => d.nodeId === nodeId)) {
      return new HttpResponse('Not found', { status: 404 })
    }
    // Delay so the modal's loading indicator is visible during dev.
    await new Promise(resolve => setTimeout(resolve, 400))
    return HttpResponse.json({
      endpoint: 1,
      entries: [
        { node: String(0x1000 + (nodeId % 16)), endpoint: 1, cluster: 6 },
      ],
    })
  }),

  http.get('/api/groupstate/:nodeId', async ({ params }) => {
    const nodeId = Number(params.nodeId)
    if (!devices.some(d => d.nodeId === nodeId)) {
      return new HttpResponse('Not found', { status: 404 })
    }
    // Delay so the modal's loading indicator is visible during dev.
    await new Promise(resolve => setTimeout(resolve, 400))
    return HttpResponse.json({
      groupKeyMap: [{ group: 4, keyset: 1 }],
      groupTable: [{ group: 4, endpoints: [1], name: '' }],
    })
  }),

  http.get('/api/threadinfo/:nodeId', async ({ params }) => {
    const nodeId = Number(params.nodeId)
    if (!devices.some(d => d.nodeId === nodeId)) {
      return new HttpResponse('Not found', { status: 404 })
    }
    // Delay so the modal's loading indicator is visible during dev.
    await new Promise(resolve => setTimeout(resolve, 400))
    // Alternate two Thread networks by node id so the split is visible in dev.
    const onOtbr = nodeId % 2 === 1
    return HttpResponse.json({
      networkName: onOtbr ? 'my-otbr' : 'HomePod-Thread',
      extendedPanId: onOtbr ? '0xDEAD00BEEF00CAFE' : '0x1122334455667788',
      panId: onOtbr ? 0x1234 : 0xABCD,
      channel: onOtbr ? 15 : 25,
      routingRole: onOtbr ? 5 : 3,
    })
  }),

  http.post('/api/identify/:nodeId', ({ params }) => {
    console.log(`[mock] identify node ${params.nodeId}`)
    return HttpResponse.json({})
  }),

  http.get('/api/nodes', () => {
    return HttpResponse.json({ nodes: nodeConfigs, edges: edgeConfigs })
  }),

  http.put('/api/nodes/:nodeId', async ({ params, request }) => {
    const id = params.nodeId as string
    const body = (await request.json()) as { x: number; y: number; settings?: Record<string, unknown> }
    const existing = nodeConfigs.find(n => n.id === id)
    if (existing) {
      existing.x = body.x
      existing.y = body.y
      if (body.settings) existing.settings = { ...existing.settings, ...body.settings }
    } else {
      nodeConfigs.push({ id, x: body.x, y: body.y, settings: body.settings ?? {} })
    }
    return HttpResponse.json({})
  }),

  http.delete('/api/nodes/:nodeId', ({ params }) => {
    const id = params.nodeId as string
    nodeConfigs = nodeConfigs.filter(n => n.id !== id)
    edgeConfigs = edgeConfigs.filter(e => e.source !== id && e.target !== id)
    return HttpResponse.json({})
  }),

  http.post('/api/edges', async ({ request }) => {
    const body = (await request.json()) as EdgeConfig
    const existing = edgeConfigs.find(e => e.id === body.id)
    if (!existing) edgeConfigs.push(body)
    return HttpResponse.json({})
  }),

  http.delete('/api/edges/:edgeId', ({ params }) => {
    const id = params.edgeId as string
    edgeConfigs = edgeConfigs.filter(e => e.id !== id)
    return HttpResponse.json({})
  }),

  http.post('/api/bindings', async ({ request }) => {
    const body = (await request.json()) as BindingConfig
    if (typeof body?.switchNodeId !== 'number') {
      return new HttpResponse('Missing switchNodeId', { status: 400 })
    }
    if (typeof body.groupId === 'number') {
      const exists = bindings.some(b => b.switchNodeId === body.switchNodeId && b.groupId === body.groupId)
      if (!exists) bindings.push({ switchNodeId: body.switchNodeId, groupId: body.groupId })
      console.log('[mock] group-bind switch', body.switchNodeId.toString(16),
        `(ep ${body.switchEndpoint ?? 'auto'})`, '-> group', body.groupId)
      return HttpResponse.json({ ok: true })
    }
    if (typeof body.lightNodeId !== 'number') {
      return new HttpResponse('Missing lightNodeId/groupId', { status: 400 })
    }
    const exists = bindings.some(b => b.switchNodeId === body.switchNodeId && b.lightNodeId === body.lightNodeId)
    if (!exists) bindings.push({ switchNodeId: body.switchNodeId, lightNodeId: body.lightNodeId })
    console.log('[mock] bind switch', body.switchNodeId.toString(16), `(ep ${body.switchEndpoint ?? 'auto'})`,
      '-> light', body.lightNodeId.toString(16), `(ep ${body.lightEndpoint ?? 'auto'})`)
    return HttpResponse.json({ ok: true })
  }),

  http.delete('/api/bindings', async ({ request }) => {
    const body = (await request.json()) as BindingConfig
    if (typeof body.groupId === 'number') {
      bindings = bindings.filter(b => !(b.switchNodeId === body.switchNodeId && b.groupId === body.groupId))
      console.log('[mock] group-unbind switch', body.switchNodeId?.toString(16), '-> group', body.groupId)
    } else {
      bindings = bindings.filter(b => !(b.switchNodeId === body.switchNodeId && b.lightNodeId === body.lightNodeId))
      console.log('[mock] unbind switch', body.switchNodeId?.toString(16), `(ep ${body.switchEndpoint ?? 'auto'})`,
        '-> light', body.lightNodeId?.toString(16), `(ep ${body.lightEndpoint ?? 'auto'})`)
    }
    return HttpResponse.json({ ok: true })
  }),

  // ---- Matter groups -------------------------------------------------------
  http.post('/api/groups', async () => {
    const groupId = nextGroupId++
    groupMembers[groupId] = new Set()
    console.log('[mock] create group', groupId)
    return HttpResponse.json({ groupId, keysetId: groupId })
  }),

  http.post('/api/groups/:groupId/members', async ({ params, request }) => {
    const groupId = Number(params.groupId)
    const body = (await request.json()) as { nodeId: number; name?: string }
    if (typeof body?.nodeId !== 'number') {
      return new HttpResponse('Missing nodeId', { status: 400 })
    }
    ;(groupMembers[groupId] ??= new Set()).add(body.nodeId)
    console.log('[mock] add node', body.nodeId.toString(16), 'to group', groupId)
    return HttpResponse.json({ ok: true })
  }),

  http.delete('/api/groups/:groupId/members/:nodeId', ({ params }) => {
    const groupId = Number(params.groupId)
    const nodeId = Number(params.nodeId)
    groupMembers[groupId]?.delete(nodeId)
    console.log('[mock] remove node', nodeId.toString(16), 'from group', groupId)
    return HttpResponse.json({ ok: true })
  }),

  http.post('/api/resetgroups/:nodeId', ({ params }) => {
    const nodeId = Number(params.nodeId)
    for (const gid of Object.keys(groupMembers)) groupMembers[Number(gid)]?.delete(nodeId)
    console.log('[mock] reset groups on node', nodeId.toString(16))
    return HttpResponse.json({ ok: true })
  }),

  http.post('/api/resetacl/:nodeId', ({ params }) => {
    const nodeId = Number(params.nodeId)
    console.log('[mock] reset ACL on node', nodeId.toString(16))
    return HttpResponse.json({ ok: true })
  }),

  http.post('/api/resetbindings/:nodeId', ({ params }) => {
    const nodeId = Number(params.nodeId)
    console.log('[mock] reset bindings on node', nodeId.toString(16))
    return HttpResponse.json({ ok: true })
  }),

  http.post('/api/grouptoggle/:groupId', ({ params }) => {
    const groupId = Number(params.groupId)
    console.log('[mock] groupcast Toggle to group', groupId)
    return HttpResponse.json({ ok: true })
  }),

  http.delete('/api/groups/:groupId', ({ params }) => {
    const groupId = Number(params.groupId)
    delete groupMembers[groupId]
    bindings = bindings.filter(b => b.groupId !== groupId)
    console.log('[mock] delete group', groupId)
    return HttpResponse.json({ ok: true })
  }),

  http.get('/api/thread/credentials', () => {
    return HttpResponse.json(threadCredentials)
  }),

  http.delete('/api/thread/credentials', () => {
    threadCredentials = { hasCredentials: false }
    return HttpResponse.json({})
  }),

  http.get('/api/thread/borderagents', () => {
    return HttpResponse.json({ agents: borderAgents })
  }),

  http.post('/api/thread/credentials/fetch', async ({ request }) => {
    const body = (await request.json()) as { host: string; port: number; otpc: string }
    const agent = borderAgents.find(a => a.host === body.host)
    threadCredentials = {
      hasCredentials: true,
      networkName: agent?.networkName ?? 'Thread network',
      channel: 15,
      panId: 0x1234,
      extPanId: 'DEAD00BEEF00CAFE',
      fetchedAt: Math.floor(Date.now() / 1000),
      borderAgentHost: body.host,
    }
    return HttpResponse.json(threadCredentials)
  }),

  controllerWs.addEventListener('connection', ({ client }) => {
    // Simulate a new device being commissioned after 10 seconds
    const timer = setTimeout(() => {
      client.send(JSON.stringify({
        type: 'device_commissioned',
        data: { nodeId: 0x3001, vendorName: 'Shelly', productName: 'Plug S' },
      }))
      devices.push({ nodeId: 0x3001, vendorName: 'Shelly', productName: 'Plug S', deviceType: 0x010a })
    }, 10000)

    client.addEventListener('close', () => clearTimeout(timer))
  }),
]
