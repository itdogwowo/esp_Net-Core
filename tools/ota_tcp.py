#!/usr/bin/env python3
"""NC4 OTA over TCP (P4 ETH :6000)。"""
import argparse, binascii, hashlib, socket, struct, time, sys

SOF=b"NC"
CMD_BEGIN=0x2201; CMD_WRITE=0x2202; CMD_END=0x2203; CMD_ACK=0x2204
CMD_CAPS=0x2210; CMD_CAPS_RSP=0x2211; CMD_STATE=0x221B; CMD_ERROR=0x221C; CMD_APPLY=0x2220

def pack(cmd,p=b"",addr=0xffff):
    b=struct.pack("<BHHH",4,addr,cmd,len(p))+p
    return SOF+b+struct.pack("<I",binascii.crc32(b)&0xffffffff)

class Reader:
    def __init__(self,s): self.s=s; self.buf=b""
    def frame(self,cmds,timeout=10):
        t0=time.time()
        while time.time()-t0<timeout:
            while len(self.buf)>=13:
                i=self.buf.find(SOF)
                if i<0: self.buf=b""; break
                self.buf=self.buf[i:]
                if len(self.buf)<13: break
                ln=struct.unpack_from("<H",self.buf,7)[0]
                if len(self.buf)<9+ln+4: break
                body=self.buf[2:9+ln]; crc=struct.unpack_from("<I",self.buf,9+ln)[0]
                cmd=struct.unpack_from("<H",self.buf,5)[0]
                f=self.buf[:9+ln+4]; self.buf=self.buf[9+ln+4:]
                if (binascii.crc32(body)&0xffffffff)==crc and cmd in cmds:
                    return cmd,f
            self.s.settimeout(max(.05,timeout-(time.time()-t0)))
            try:
                d=self.s.recv(65536)
            except socket.timeout:
                break
            if not d: break
            self.buf+=d
        return None,None

# 韌體的 OTA_BEGIN 會先把目標分區**整段抹掉**才回 ACK（esp_ota_begin 同步 erase）。
# S3 上實測 ≈237 KB/s：1.2 MB 要 ~5.2 s，剛好越過舊版硬編的 5 s 等待 →
# 「BEGIN no reply」假故障（2026-11-17）。這裡照 image 大小估，兩種通道同一條規則。
def begin_timeout(nbytes):
    return 10.0 + nbytes / 100_000.0

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--host",default="10.10.1.50")
    ap.add_argument("--port",type=int,default=6000)
    ap.add_argument("--chunk",type=int,default=4096)
    ap.add_argument("--no-apply",action="store_true")
    ap.add_argument("--delay-ms",type=int,default=500)
    ap.add_argument("image")
    a=ap.parse_args()
    data=open(a.image,"rb").read(); digest=hashlib.sha256(data).digest()
    ver=("ota-"+time.strftime("%Y%m%d-%H%M%S")).encode()
    print(f"image {a.image} {len(data)} B")
    s=socket.create_connection((a.host,a.port),timeout=5); s.settimeout(5)
    rd=Reader(s)
    s.sendall(pack(CMD_CAPS)); cmd,f=rd.frame({CMD_CAPS_RSP},2)
    if f:
        max_chunk,=struct.unpack_from("<H",f,9); print("caps max_chunk",max_chunk)
        if a.chunk>max_chunk: a.chunk=max_chunk
    tmo=begin_timeout(len(data))
    print(f"BEGIN（先抹 {len(data)/1024:.0f} KB，估 ≤ {tmo:.0f} s）")
    s.sendall(pack(CMD_BEGIN,struct.pack("<IH",len(data),a.chunk)+digest+struct.pack("<H",len(ver))+ver))
    cmd,f=rd.frame({CMD_ACK,CMD_ERROR},tmo)
    if cmd!=CMD_ACK:
        # 回覆可能只是遲到：問 STATE 確認係咪已經入咗 writing。
        s.sendall(pack(CMD_STATE)); scmd,sf=rd.frame({CMD_STATE},3)
        if scmd==CMD_STATE and len(sf)>=14 and sf[9+1]:
            print("BEGIN: 冇收到 ACK，但 STATE=writing → 當作已開始，繼續")
        else:
            print("BEGIN failed", f.hex() if f else "no reply"); return 1
    print("BEGIN ok")
    off=0; t0=time.time()
    while off<len(data):
        chunk=data[off:off+a.chunk]
        s.sendall(pack(CMD_WRITE,struct.pack("<I",off)+chunk))
        cmd,f=rd.frame({CMD_ACK,CMD_ERROR},5)
        if cmd!=CMD_ACK:
            print("\nWRITE failed", (f.hex() if f else "no reply"), "@", off); return 1
        ack_off,written=struct.unpack_from("<II",f,9)
        if ack_off!=off:
            print("offset echo mismatch",ack_off,off); return 1
        off=written
        print(f"\r  {off}/{len(data)} {100*off/len(data):5.1f}%",end="",flush=True)
    print(f"\nWRITE ok {time.time()-t0:.1f}s")
    s.sendall(pack(CMD_END)); cmd,f=rd.frame({CMD_STATE,CMD_ERROR},15)
    if cmd!=CMD_STATE:
        print("END failed",f.hex() if f else "no reply"); return 1
    print("END ok")
    if a.no_apply: return 0
    s.sendall(pack(CMD_APPLY,struct.pack("<BI",0,a.delay_ms)))
    rd.frame({CMD_STATE},3)
    print("APPLY sent")
    s.close(); return 0

if __name__=="__main__": raise SystemExit(main())
