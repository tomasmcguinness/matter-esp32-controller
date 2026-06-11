import { NavLink, Routes, Route } from 'react-router'
import './App.css'
import Canvas from './Canvas.tsx'
import Devices from './Devices.tsx'
import Thread from './Thread.tsx'

import '@xyflow/react/dist/style.css';

function App() {
  return (
    <>
      <nav className="navbar navbar-expand-lg">
        <div className="container">
          <a className="navbar-brand" href="#">Matter Controller</a>
          <button className="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarNav" aria-controls="navbarNav" aria-expanded="false" aria-label="Toggle navigation">
            <span className="navbar-toggler-icon"></span>
          </button>
          <div className="collapse navbar-collapse" id="navbarNav">
            <ul className="navbar-nav">
              <li className="nav-item">
                <NavLink className="nav-link" to="/">Canvas</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/devices">Devices</NavLink>
              </li>
              <li className="nav-item">
                <NavLink className="nav-link" to="/thread">Thread</NavLink>
              </li>
            </ul>
          </div>
        </div>
      </nav>
      <Routes>
        <Route path="/" element={<Canvas />} />
        <Route path="/devices" element={<div className="container"><Devices /></div>} />
        <Route path="/thread" element={<div className="container"><Thread /></div>} />
      </Routes>
    </>
  )
}

export default App
