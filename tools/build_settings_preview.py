"""Build a local, offline preview from the actual embedded Web UI.

The preview uses fixtures and blocks device mutations. It is never flashed.
Run from the repository root: python tools/build_settings_preview.py
"""
from pathlib import Path

source = Path('src/web/web_ui.h').read_text(encoding='utf-8')
html = source.split('R"rawliteral(', 1)[1].rsplit(')rawliteral";', 1)[0]
fixtures = r'''
<script>
// Preview only: no requests are forwarded to a device or another server.
const previewGuard={mod_ms:0,key_ms:60000,voice_ms:900000,forced:0,last_reason:''};
const previewPower={timeout_enabled:true,timeout_min:2};
const previewLayers=Array.from({length:5},(_,id)=>({id,name:id?'层'+id:'默认层',type:0,timeout:0,color:id?'0x06B6D4':'0x00FF00',bindings:[]}));
const previewLogs=Array.from({length:50},(_,i)=>'[示例 '+String(i).padStart(2,'0')+':00] '+['BLE 遥控器连接正常','UAC 主机麦克风待机','HID 按键释放完成','SYSTEM 运行状态正常'][i%4]);
window.fetch=async (url,options={})=>{
  if((options.method||'GET').toUpperCase()!=='GET')return new Response(JSON.stringify({error:'预览不修改设备',message:'预览未执行操作'}),{status:403,headers:{'Content-Type':'application/json'}});
  const path=String(url).split('?')[0];
  const data={
    '/api/status':{version:'预览示例',uptime_sec:3600,frames_decoded:412,free_heap:65536,free_psram:8192000,sta_connected:true,sta_ssid:'示例家庭 Wi-Fi',sta_ip:'192.168.1.100',sta_rssi:-60,ap_running:false,ap_ip:'192.168.4.1',ap_secured:false,ap_pass:'',guard:previewGuard},
    '/api/ble/info':{connected:true,name:'小米蓝牙语音遥控器（示例）',mac:'AA:BB:CC:DD:EE:01',battery_pct:90,bound_name:'小米蓝牙语音遥控器（示例）',bound_mac:'AA:BB:CC:DD:EE:01',dev_model:'示例遥控器',dev_manufacturer:'示例制造商',dev_hw:'示例'},
    '/api/guard':previewGuard,
    '/api/power':previewPower,
    '/api/debug/gatt-dump':{enabled:false},
    '/api/keymap':{active_layer:0,layers:previewLayers},
    '/api/keymap/telemetry':{active_layer:0,source_vk:0,is_pressed:false},
    '/api/logs':{logs:previewLogs},
    '/api/ota/status':{supported:true,version:'预览示例',running_label:'app0'},
    '/api/nvs':{preview:true,message:'只读配置快照示例，未读取设备',keymap:{layers:5},wifi:{ssid:'示例家庭 Wi-Fi',password:'********'}},
    '/api/wifi/scan':{networks:[{ssid:'示例家庭 Wi-Fi',rssi:-60,auth:3}]},
    '/api/ble/scan':{devices:[]},
    '/api/config/export':{preview:true,message:'示例配置，不能用于设备恢复'}
  }[path]||{};
  return new Response(JSON.stringify(data),{headers:{'Content-Type':'application/json'}});
};
document.addEventListener('click',e=>{
  const link=e.target.closest('a[href^="http"]');
  if(link){e.preventDefault();e.stopImmediatePropagation();showToast('这是示例访问地址，预览不会打开设备后台');return;}
  const control=e.target.closest('[onclick]');if(!control)return;
  const handler=control.getAttribute('onclick');
  if(/save|reset|restart|unpair|reconnect|toggleGatt|import|exportKeymap|exportSystem|clearLogs|ota-file-input|sysconfig-file-input|keymap-file-input|connectBleDevice|testSendWol/i.test(handler)){
    e.preventDefault();e.stopImmediatePropagation();showToast('这是现有页面的预览，未执行设备操作');
  }
},true);
document.addEventListener('change',e=>{if(e.target.id==='wifi-pwr-mode'){e.stopImmediatePropagation();previewPower.timeout_enabled=e.target.checked;showToast('空闲休眠已在预览中切换，未写入设备');}},true);
</script>
'''
html=html.replace('<title>RemoteMapper - 小米蓝牙遥控器硬件桥接器</title>','<title>RemoteMapper · 现有页面设置整理预览</title><link rel="icon" href="data:,">')
html=html.replace('<body>', '<body><div style="text-align:center;font-size:12px;color:#fbbf24;padding:10px 16px;border-bottom:1px solid #243247;background:#111827">现有页面优化预览 · 全部为示例数据 · 不连接或修改设备</div>'+fixtures,1)
html=html.replace('</body>','<script>document.getElementById(\'wifi-ssid\').value=\'示例家庭 Wi-Fi\';switchTab(\'tab-wifi\');</script>\n</body>',1)
output=Path('docs/previews/home-layout.html')
output.write_text(html,encoding='utf-8')
print(f'Built {output} from src/web/web_ui.h with offline fixtures')
