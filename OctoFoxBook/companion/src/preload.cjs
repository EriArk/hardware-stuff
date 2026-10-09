'use strict';
const {contextBridge,ipcRenderer}=require('electron');
contextBridge.exposeInMainWorld('companion',Object.freeze({
 scan:()=>ipcRenderer.invoke('companion:scan'),
 connect:id=>ipcRenderer.invoke('companion:connect',id),
 manual:value=>ipcRenderer.invoke('companion:manual',value),
 disconnect:()=>ipcRenderer.invoke('companion:disconnect'),
 navigate:where=>ipcRenderer.invoke('companion:navigate',where),
 status:callback=>ipcRenderer.on('companion:status',(_event,value)=>callback(value))
}));
