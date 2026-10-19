# swim.py [SECONDS=100] > swim.rec - the dive movement for davebot (notes/games-unity.md): swim right (D) 4 s, down
# (S) 1.5 s, left (A) 4 s, down 1.5 s, ... for SECONDS - covers both directions and a few depths so the frame times
# include scrolling and new fish. keyrec format "<ms> D|U <scan hex> <ext>"; A 1E, D 20, S 1F.
import sys
total = int(sys.argv[1]) * 1000 if len(sys.argv) > 1 else 100000
t, out, i = 1000, [], 0
steps = [('20', 4000), ('1F', 1500), ('1E', 4000), ('1F', 1500)]
while t < total:
    k, ms = steps[i % len(steps)]
    out.append('%d D %s 0' % (t, k))
    out.append('%d U %s 0' % (t + ms, k))
    t += ms + 200
    i += 1
print('# davebot swim pattern, %d s' % (total // 1000))
print('\n'.join(out))
