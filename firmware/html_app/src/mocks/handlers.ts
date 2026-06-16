import { http, HttpResponse, ws } from 'msw'
import type { CommissionedDevice } from '../Devices'
import type { ThreadCredentials, BorderAgent } from '../Thread'

type NodeConfig = { id: string; x: number; y: number; settings: Record<string, unknown> }
type EdgeConfig = { id: string; source: string; target: string; sourceHandle?: string; targetHandle?: string }

let nodeConfigs: NodeConfig[] = []
let edgeConfigs: EdgeConfig[] = []

let devices: CommissionedDevice[] = [
  {
    nodeId: 0x1001,
    vendorName: 'Philips',
    productName: 'Hue White',
    deviceType: 0x0100,
  },
  {
    nodeId: 0x1002,
    vendorName: 'IKEA',
    productName: 'TRADFRI bulb',
    deviceType: 0x0100,
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
