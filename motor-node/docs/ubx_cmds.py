import struct
def ck(data):
    cka=ckb=0
    for b in data:
        cka=(cka+b)&0xFF
        ckb=(ckb+cka)&0xFF
    return cka,ckb
def ubx(cls,iid,payload=b""):
    b=bytes([0xB5,0x62,cls,iid,len(payload)&0xFF,(len(payload)>>8)&0xFF])+payload
    a,z=ck(b[2:])
    return b+bytes([a,z])
def show(name,b):
    print(f"{name}:")
    print("   "+" ".join(f"{x:02X}" for x in b))
    print("   (%d bytes)"%len(b))

show("MON-VER   request (0A 04)", ubx(0x0A,0x04))
show("MON-HW    request (0A 09)", ubx(0x0A,0x09))
show("CFG-GNSS  request (06 3E)", ubx(0x06,0x3E))
show("CFG-RATE  request (06 08)", ubx(0x06,0x08))
show("NAV-PVT   request (01 07)", ubx(0x01,0x07))

# CFG-PRT UART1 (portId=1), 8N1 no flow, UBX+NMEA, 9600
mode=0x000008D0
p=struct.pack('<BBBI',1,0,0,mode)+bytes(4)+struct.pack('<I',9600)+struct.pack('<H',0x0003)+struct.pack('<H',0x0003)+struct.pack('<H',0x0000)+bytes(2)
show("CFG-PRT UART1 set baud=9600", ubx(0x06,0x00,p))
p2=struct.pack('<BBBI',1,0,0,mode)+bytes(4)+struct.pack('<I',115200)+struct.pack('<H',0x0003)+struct.pack('<H',0x0003)+struct.pack('<H',0x0000)+bytes(2)
show("CFG-PRT UART1 set baud=115200", ubx(0x06,0x00,p2))
# CFG-PRT query UART1 only
show("CFG-PRT  request UART1", ubx(0x06,0x00,bytes([1])))

show("CFG-RATE meas=1000ms nav=1", ubx(0x06,0x08,struct.pack('<HHH',1000,1,0)))
show("CFG-RATE meas=200ms(5Hz) nav=1", ubx(0x06,0x08,struct.pack('<HHH',200,1,0)))

for nm,iid in {'GGA':0x00,'GSA':0x02,'GSV':0x03,'RMC':0x04,'VTG':0x05}.items():
    show(f"CFG-MSG enable {nm} on UART1 (rate=1)", ubx(0x06,0x01,bytes([0xF0,iid,0,0,0,0,1])))
    show(f"CFG-MSG disable {nm} on UART1 (rate=0)", ubx(0x06,0x01,bytes([0xF0,iid,0,0,0,0,0])))

def gnss_block(gnssId,resTrk,maxTrk,en,sigmask):
    return bytes([gnssId,resTrk,maxTrk,0])+struct.pack('<I',(1 if en else 0)|(sigmask<<16))
# 3-block: GPS + GLONASS + Galileo (M8 can do 3 concurrently)
b=[gnss_block(0,7,16,1,0x0001), gnss_block(6,6,10,1,0x0001), gnss_block(2,6,8,1,0x0001)]
show("CFG-GNSS enable GPS+GLONASS+Galileo (3)", ubx(0x06,0x3E,struct.pack('<BBBB',0,20,16,3)+b"".join(b)))
# 4-block incl Beidou (likely exceeds M8 HW)
b=[gnss_block(0,7,16,1,1),gnss_block(2,6,8,1,1),gnss_block(3,6,8,1,1),gnss_block(6,6,10,1,1)]
show("CFG-GNSS attempt GPS+Galileo+BeiDou+GLONASS (4!)", ubx(0x06,0x3E,struct.pack('<BBBB',0,20,16,4)+b"".join(b)))

show("CFG-CFG save flash+BBR", ubx(0x06,0x09,struct.pack('<III',0x010,0x0010,0x0011)))
show("CFG-CFG save to flash", ubx(0x06,0x09,struct.pack('<III',0x00,0x00,0x01)))
show("CFG-RST COLD start", ubx(0x06,0x04,struct.pack('<HBB',0xFFFF,0x00,0)))
show("CFG-RST warm start", ubx(0x06,0x04,struct.pack('<HBB',0xFFDF,0x01,0)))