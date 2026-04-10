import { useEffect, useRef, useState, useCallback } from 'react';
import CodeMirror from '@uiw/react-codemirror';
import { javascript } from '@codemirror/lang-javascript';
import { oneDark } from '@codemirror/theme-one-dark';
import { Monitor, Users, FileCode2, Code2, Plus, Trash2, Edit2, FolderOpen } from 'lucide-react';
import './App.css';

type FileData = { id: number; name: string; content: string };
type Toast = { id: number; message: string };

function App() {
  const [files, setFiles] = useState<FileData[]>([]);
  const [activeFileId, setActiveFileId] = useState<number | null>(null);
  const [users, setUsers] = useState<string[]>([]);
  const [username, setUsername] = useState<string | null>(null);
  const [status, setStatus] = useState<'connecting' | 'connected' | 'disconnected'>('connecting');
  const [toasts, setToasts] = useState<Toast[]>([]);
  
  const [modalConfig, setModalConfig] = useState<{
    type: 'prompt' | 'confirm';
    title: string;
    onConfirm: (val: string) => void;
  } | null>(null);
  const [modalInput, setModalInput] = useState('');
  
  const ws = useRef<WebSocket | null>(null);
  const toastIdRef = useRef(0);

  const addToast = (message: string) => {
    const id = ++toastIdRef.current;
    setToasts(prev => [...prev, { id, message }]);
    setTimeout(() => {
      setToasts(prev => prev.filter(t => t.id !== id));
    }, 3000);
  };

  useEffect(() => {
    const connectWs = () => {
      const socket = new WebSocket('ws://<SERVERIP>:8080');
      ws.current = socket;

      socket.onopen = () => {
        setStatus('connected');
        if (username) {
          socket.send(JSON.stringify({ type: 'join', username }));
        }
      };
      
      socket.onclose = () => setStatus('disconnected');
      socket.onerror = () => setStatus('disconnected');

      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          
          if (data.type === 'init') {
            setFiles(data.files || []);
          } else if (data.type === 'users') {
            setUsers(data.users || []);
          } else if (data.type === 'toast') {
            addToast(data.message);
          } else if (data.type === 'edit') {
            setFiles(prev => prev.map(f => f.id === data.fileId ? { ...f, content: data.content } : f));
          } else if (data.type === 'create_res') {
            setFiles(prev => [...prev, { id: data.fileId, name: data.name, content: '// new file' }]);
          } else if (data.type === 'delete') {
            setFiles(prev => prev.filter(f => f.id !== data.fileId));
            setActiveFileId(prev => prev === data.fileId ? null : prev);
          } else if (data.type === 'rename') {
            setFiles(prev => prev.map(f => f.id === data.fileId ? { ...f, name: data.name } : f));
          }
        } catch (e) {
          console.error("Failed to parse websocket JSON", e, event.data);
        }
      };
    };

    connectWs();
    return () => ws.current?.close();
  }, [username]); // Re-bind if username formally registers

  const handleLoginSubmit = (e: React.FormEvent<HTMLFormElement>) => {
    e.preventDefault();
    const fd = new FormData(e.currentTarget);
    const name = fd.get('username') as string;
    if (name && name.trim().length > 0) {
      setUsername(name.trim());
    }
  };

  const handleChange = useCallback((val: string, viewUpdate: any) => {
    setFiles(prev => prev.map(f => f.id === activeFileId ? { ...f, content: val } : f));
    
    const isUserDriven = viewUpdate.transactions.some((tr: any) => 
      tr.isUserEvent('input') || tr.isUserEvent('delete') || tr.isUserEvent('undo') || tr.isUserEvent('redo') || tr.isUserEvent('paste')
    );
    
    if (isUserDriven && ws.current?.readyState === WebSocket.OPEN && activeFileId !== null) {
      ws.current.send(JSON.stringify({ type: 'edit', fileId: activeFileId, content: val }));
    }
  }, [activeFileId]);

  const handleCreateFile = () => {
    setModalConfig({
      type: 'prompt',
      title: 'Enter new file name',
      onConfirm: (name) => {
        if (name && name.trim().length > 0 && ws.current?.readyState === WebSocket.OPEN) {
          ws.current.send(JSON.stringify({ type: 'create', name: name.trim() }));
        }
      }
    });
    setModalInput('');
  };

  const handleDeleteFile = (e: React.MouseEvent, id: number) => {
    e.stopPropagation();
    setModalConfig({
      type: 'confirm',
      title: 'Are you sure you want to delete this file?',
      onConfirm: () => {
        if (ws.current?.readyState === WebSocket.OPEN) {
          ws.current.send(JSON.stringify({ type: 'delete', fileId: id }));
        }
      }
    });
  };

  const handleRenameFile = (e: React.MouseEvent, id: number, oldName: string) => {
    e.stopPropagation();
    setModalConfig({
      type: 'prompt',
      title: 'Enter new file name',
      onConfirm: (newName) => {
        if (newName && newName.trim().length > 0 && newName !== oldName && ws.current?.readyState === WebSocket.OPEN) {
          ws.current.send(JSON.stringify({ type: 'rename', fileId: id, name: newName.trim() }));
        }
      }
    });
    setModalInput(oldName);
  };

  const activeFile = files.find(f => f.id === activeFileId);

  if (!username) {
    return (
      <div className="login-gate">
        <div className="login-box">
          <Monitor color="#c9d1d9" size={48} />
          <h2>Welcome to Codesync</h2>
          <p>Please enter your display name to join the workspace.</p>
          <form className="login-form" onSubmit={handleLoginSubmit}>
            <input name="username" type="text" placeholder="e.g. Satoshi" autoFocus required maxLength={30} />
            <button type="submit">Join Workspace</button>
          </form>
        </div>
      </div>
    );
  }

  return (
    <div className="app-container">
      <header className="header">
        <div className="logo-container">
          <div className="logo-icon">
            <Monitor color="#c9d1d9" size={20} />
          </div>
          <span className="title">Codesync</span>
        </div>
        
        <div className="status-panel">
          <div className="user-count-badge dropdown-trigger">
            <Users size={16} /> 
            <span>{users.length} {users.length === 1 ? 'User' : 'Users'} Online</span>
            <div className="dropdown-menu">
              <div className="dropdown-header">Active Collaborators</div>
              {users.map((u, i) => (
                <div key={i} className="dropdown-item">
                  <span className="user-dot"></span> {u} {u === username ? '(You)' : ''}
                </div>
              ))}
            </div>
          </div>
          <div className={`status-badge ${status}`}>
            <span className="pulse"></span>
            {status === 'connected' ? 'Connected' : status === 'connecting' ? 'Connecting...' : 'Disconnected'}
          </div>
        </div>
      </header>

      <div className="main-content">
        <aside className="sidebar">
          <div className="sidebar-header">
            <span>Explorer</span>
            <button className="icon-btn" onClick={handleCreateFile} title="New File">
              <Plus size={16} />
            </button>
          </div>
          <div className="file-list">
            {files.map(file => (
              <div 
                key={file.id} 
                className={`sidebar-item ${activeFileId === file.id ? 'active' : ''}`}
                onClick={() => setActiveFileId(file.id)}
              >
                <div className="file-info">
                  {file.name.endsWith('.js') || file.name.endsWith('.ts') ? (
                    <FileCode2 size={16} color="inherit" />
                  ) : (
                    <Code2 size={16} color="inherit" />
                  )}
                  <span className="file-name">{file.name}</span>
                </div>
                <div className="file-actions">
                  <button className="icon-btn-small" onClick={(e) => handleRenameFile(e, file.id, file.name)} title="Rename">
                    <Edit2 size={12} />
                  </button>
                  <button className="icon-btn-small danger" onClick={(e) => handleDeleteFile(e, file.id)} title="Delete">
                    <Trash2 size={12} />
                  </button>
                </div>
              </div>
            ))}
            {files.length === 0 && (
              <div className="empty-files">No files in directory.</div>
            )}
          </div>
        </aside>

        <main className="editor-workspace">
          {activeFileId !== null && activeFile ? (
            <>
              <div className="editor-tabs">
                <div className="tab active-tab">
                  {activeFile.name.endsWith('.js') ? <FileCode2 size={14} /> : <Code2 size={14} />} 
                  {activeFile.name}
                </div>
              </div>
              
              <div className="editor-container">
                <CodeMirror
                  value={activeFile.content}
                  height="100%"
                  theme={oneDark}
                  extensions={[javascript({ jsx: true })]}
                  onChange={handleChange}
                  className="cm-editor"
                />
              </div>
            </>
          ) : (
            <div className="landing-page">
              <div className="landing-content">
                <div className="landing-icon-wrapper">
                  <FolderOpen size={64} strokeWidth={1} />
                </div>
                <h2>Welcome to Codesync</h2>
                <p>Select a file from the explorer sidebar to start editing synchronously, or create a brand new one to share with your team instantly.</p>
                
                <button className="primary-btn" onClick={handleCreateFile}>
                  <Plus size={18} /> Create New File
                </button>
              </div>
            </div>
          )}
        </main>
      </div>

      {modalConfig && (
        <div className="modal-overlay" onClick={() => setModalConfig(null)}>
          <div className="modal-content" onClick={e => e.stopPropagation()}>
            <h3>{modalConfig.title}</h3>
            {modalConfig.type === 'prompt' && (
              <input 
                type="text" 
                className="modal-input" 
                value={modalInput} 
                onChange={e => setModalInput(e.target.value)}
                onKeyDown={e => {
                  if (e.key === 'Enter') {
                    modalConfig.onConfirm(modalInput);
                    setModalConfig(null);
                  }
                }}
                autoFocus
              />
            )}
            <div className="modal-actions">
              <button className="secondary-btn" onClick={() => setModalConfig(null)}>Cancel</button>
              <button 
                className={modalConfig.type === 'confirm' ? 'danger-btn' : 'primary-btn'} 
                onClick={() => {
                  modalConfig.onConfirm(modalInput);
                  setModalConfig(null);
                }}
              >
                {modalConfig.type === 'confirm' ? 'Delete' : 'Confirm'}
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Floating Toast Notification Container */}
      <div className="toast-container">
        {toasts.map(t => (
          <div key={t.id} className="toast">
            {t.message}
          </div>
        ))}
      </div>
    </div>
  );
}

export default App;
