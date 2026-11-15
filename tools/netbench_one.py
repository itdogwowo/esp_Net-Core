import argparse,json,socket,struct,time,sys
sys.path.insert(0,'tools')
from nc4_demux import pack
CMD=0xF040
def start(): return pack(CMD,b'\x00',0xffff)
def data(seq,p): return pack(CMD,b'\x03'+struct.pack('<I',seq)+p,0xffff)
def result(): return pack(CMD,b'\x02',0xffff)
def parse(buf):
 out=[]
 while len(buf)>=9:
  i=buf.find(b'NC')
  if i<0: return out,b''
  buf=buf[i:]
  if len(buf)<9: return out,buf
  ln=struct.unpack_from('<H',buf,7)[0]
  if len(buf)<9+ln+4: return out,buf
  fr=buf[:9+ln+4]; buf=buf[9+ln+4:]
  try:
   j=json.loads(fr[11:9+ln].decode())
   out.append(j)
  except: pass
 return out,buf
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--ip',required=True); ap.add_argument('--mode',choices=['tcp','udp','ws'],default='tcp'); ap.add_argument('--port',type=int,default=0); ap.add_argument('--count',type=int,default=1000); ap.add_argument('--payload',type=int,default=8192); ap.add_argument('--batch',type=int,default=1); ap.add_argument('--timeout',type=float,default=10); a=ap.parse_args()
 port=a.port or {'tcp':6000,'udp':6001,'ws':8080}[a.mode]
 p=b'X'*a.payload; frames=[data(i,p) for i in range(a.count)]
 if a.mode=='tcp':
  s=socket.create_connection((a.ip,port),timeout=3); s.settimeout(2); send=s.sendall; recv=lambda n=65536:s.recv(n)
 elif a.mode=='udp':
  s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(2); send=lambda b:s.sendto(b,(a.ip,port)); recv=lambda n=65536:s.recvfrom(n)[0]
 else:
  import websocket
  s=websocket.create_connection(f'ws://{a.ip}:{port}/nc4',timeout=3); s.settimeout(2); send=s.send_binary; recv=lambda n=65536:s.recv()
 send(start());
 try: recv()
 except: pass
 t0=time.time()
 for i in range(0,len(frames),max(1,a.batch)):
  blob=b''.join(frames[i:i+max(1,a.batch)])
  if a.mode=='udp' and len(blob)>1200:
   for fr in frames[i:i+max(1,a.batch)]: send(fr)
  else: send(blob)
 send_s=time.time()-t0
 res=None; deadline=time.time()+a.timeout; buf=b''
 while time.time()<deadline:
  send(result())
  try: d=recv()
  except Exception as e: time.sleep(.1); continue
  if isinstance(d,str): d=d.encode()
  buf+=d
  js,buf=parse(buf)
  for j in js:
   if 'frames' in j: res=j; break
  if res: break
  time.sleep(.2)
 wall=time.time()-t0
 out={'mode':a.mode,'count':a.count,'payload':a.payload,'batch':a.batch,'send_s':round(send_s,4),'wall_s':round(wall,4),'result':res}
 if res:
  us=res.get('us') or 0
  out['ok']=res.get('frames')==a.count and res.get('lost',0)==0
  out['speed_mbps']=round(res['bytes']*1e6/us/1024/1024,3) if us else 0
  out['wall_mbps']=round(res['bytes']/wall/1024/1024,3) if wall else 0
 print(json.dumps(out,ensure_ascii=False))
 s.close()
if __name__=='__main__': main()
