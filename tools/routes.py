import json, math
HOME=(51.0447,-114.0719); LS=math.cos(math.radians(HOME[0])); KM=111.32
def xy(la,lo): return ((lo-HOME[1])*KM*LS, (la-HOME[0])*KM)
def dist(la,lo):
    x,y=xy(la,lo); return math.hypot(x,y)
def plen(p):
    return sum(math.hypot(*(lambda a,b:(a[0]-b[0],a[1]-b[1]))(xy(*p[i]),xy(*p[i-1]))) for i in range(1,len(p)))

ways=json.load(open('map_ordered.json'))
# Stitch drivable ways (MAJOR=3, ROAD=2) into long chains for plow routes.
cand=[w['p'][:] for w in ways if w['c'] in (2,3)]   # MAP_ROAD, MAP_MAJOR
key=lambda p:(round(p[0],5),round(p[1],5))
ends={}
for i,p in enumerate(cand):
    ends.setdefault(key(p[0]),[]).append((i,0))
    ends.setdefault(key(p[-1]),[]).append((i,1))

used=[False]*len(cand); chains=[]
for i in range(len(cand)):
    if used[i]: continue
    used[i]=True; ch=cand[i][:]
    for side in (0,1):
        while True:
            end = ch[0] if side==0 else ch[-1]
            nxt=None
            for (j,e) in ends.get(key(end),[]):
                if not used[j]: nxt=(j,e); break
            if not nxt: break
            j,e=nxt; used[j]=True
            seg=cand[j][:] 
            if e==1: seg.reverse()
            if side==0: ch = seg[:-1] + ch
            else:       ch = ch + seg[1:]
    chains.append(ch)

routes=[]
for ch in chains:
    if len(ch)<4: continue
    near=min(dist(*p) for p in ch)
    L=plen(ch)
    if near<11.0 and L>6.0: routes.append((L,near,ch))
routes.sort(key=lambda r:(r[1],-r[0]))
routes=routes[:8]
print('plow routes:', len(routes))
for L,n,ch in routes: print('   len %5.1f km  closest %4.1f km  pts %d' % (L,n,len(ch)))
json.dump([r[2] for r in routes], open('routes.json','w'), separators=(',',':'))

# compact map payload for the preview
json.dump([[w['c'], [c for p in w['p'] for c in p]] for w in ways],
          open('map_flat.json','w'), separators=(',',':'))
print('map_flat:', len(open('map_flat.json').read()), 'bytes;',
      'routes:', len(open('routes.json').read()), 'bytes')
