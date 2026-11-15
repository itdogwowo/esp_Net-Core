import argparse,socket,time,json
ap=argparse.ArgumentParser(); ap.add_argument('--ip',required=True); ap.add_argument('--total',type=int,default=4*1024*1024); a=ap.parse_args()
def upload(chunk):
 s=socket.create_connection((a.ip,5000),timeout=5); s.settimeout(15)
 data=b'X'*chunk; sent=0; t0=time.time()
 while sent<a.total:
  n=min(chunk,a.total-sent); s.sendall(data[:n]); sent+=n
 dt=time.time()-t0; s.close(); return a.total/dt/1024/1024
def download(chunk):
 s=socket.create_connection((a.ip,5001),timeout=5); s.settimeout(15)
 s.sendall(f'GO {a.total}\n'.encode()); got=0; t0=time.time(); buf=bytearray(65536)
 while got<a.total:
  n=s.recv_into(buf,min(len(buf),a.total-got))
  if n<=0: break
  got+=n
 dt=time.time()-t0; s.close(); return got/dt/1024/1024, got
out={'ip':a.ip,'total':a.total,'upload':{},'download':{}}
for k in (2048,4096,8192,16384):
 try: out['upload'][k]=round(upload(k),3)
 except Exception as e: out['upload'][k]='ERR '+str(e)
 try:
  mb,got=download(k); out['download'][k]={'mbps':round(mb,3),'got':got}
 except Exception as e: out['download'][k]='ERR '+str(e)
print(json.dumps(out,ensure_ascii=False))
