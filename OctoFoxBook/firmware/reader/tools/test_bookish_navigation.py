"""Check styled sections and nested Back on a reader; captures contain local book metadata."""
from pathlib import Path
import argparse
import hashlib
import re
import time
from test_home_navigation import open_connection, transact
from capture_display import png_from_native
from provision_reader import resume_sync

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',default='COM3');p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    headers=[]
    with open_connection(args.port) as c:
        def step(command,marker):return transact(c,command,marker,35)
        def key(button,marker,gesture='SHORT'):return step(f'INPUT {button} {gesture}',marker)
        def library(phase,selection=None):
            status=step('LIBRARY STATUS','LIBRARY STATUS')[-1]
            assert f'phase={phase}' in status,status
            if selection is not None:assert re.search(r'selected=(\d+)',status)[1]==str(selection),status
            return status
        def capture(name):
            step('DISPLAY CAPTURE','DISPLAY CAPTURE BEGIN')
            data=bytearray();deadline=time.monotonic()+30
            while len(data)<259200 and time.monotonic()<deadline:data.extend(c.read(min(4096,259200-len(data))))
            assert len(data)==259200
            assert b'DISPLAY CAPTURE COMPLETE' in c.read_until(b'DISPLAY CAPTURE COMPLETE\r\n')
            (args.output/f'{name}.png').write_bytes(png_from_native(data))
            # Logo/header above tabs must be pixel-identical in every section.
            header=bytes(((data[x*480+(959-y)//2]>>(4 if (959-y)&1 else 0))&15)
                for y in range(100) for x in range(540))
            headers.append(hashlib.sha256(header).hexdigest())
            print('CAPTURE',name,'header_sha256='+headers[-1])
        try:
            step('PING','PONG')
            step('LIBRARY OPEN','LIBRARY OPEN COMPLETE');library(0);capture('library')
            key('CENTER','LIBRARY OPEN COMPLETE');library(2);capture('books')
            key('UP','LIBRARY OPEN COMPLETE');capture('back')
            key('CENTER','LIBRARY OPEN COMPLETE');library(0,1)
            for _ in range(5):key('DOWN','LIBRARY OPEN COMPLETE')
            key('CENTER','LIBRARY OPEN COMPLETE');library(1);capture('authors')
            key('CENTER','LIBRARY OPEN COMPLETE');library(2)
            key('UP','LIBRARY OPEN COMPLETE');key('CENTER','LIBRARY OPEN COMPLETE');library(1)
            key('UP','LIBRARY OPEN COMPLETE');key('CENTER','LIBRARY OPEN COMPLETE');library(0,6)
            step('SEARCH OPEN','SEARCH OPEN COMPLETE');capture('search')
            key('CENTER','SEARCH OPEN COMPLETE');capture('letters')
            key('UP','SEARCH OPEN COMPLETE');key('CENTER','SEARCH OPEN COMPLETE')
            for _ in range(5):key('DOWN','SEARCH OPEN COMPLETE')
            assert 'selected=6/10' in step('SEARCH STATUS','SEARCH STATUS')[-1]
            key('CENTER','SEARCH OPEN COMPLETE');key('CENTER','LIBRARY OPEN COMPLETE')
            status=library(2);capture('results')
            if 'rows=0 ' not in status:key('UP','LIBRARY OPEN COMPLETE')
            key('CENTER','SEARCH OPEN COMPLETE')
            assert 'phase=2' in step('SEARCH STATUS','SEARCH STATUS')[-1]
            key('CENTER','SECTIONS OPEN COMPLETE',gesture='LONG');capture('tabs')
            key('DOWN','SECTIONS OPEN COMPLETE');key('CENTER','FAVORITES OPEN');capture('favorites')
            opened=key('CENTER','FAVORITES OPEN')[-1];capture('folder')
            if 'count=0 ' not in opened:key('UP','FAVORITES OPEN')
            key('CENTER','FAVORITES OPEN phase=0')
            assert 'radio=off' in step('WIFI STATUS','WIFI STATUS')[-1]
            assert len(set(headers))==1,headers
            step('LIBRARY OPEN','LIBRARY OPEN COMPLETE')
            for _ in range(5):key('UP','LIBRARY OPEN COMPLETE')
            key('CENTER','LIBRARY OPEN COMPLETE')
            print('BOOKISH_NAVIGATION_ACCEPTED header_identical=true back=true groups=true latin_search=true favorites=true radio=off')
        finally:resume_sync(c)
if __name__=='__main__':main()
