"""Boot the game straight into a race on a given track: no menus, racing in about 10 s.

  py -3 tools/quickrace.py [--track ID] [--car ID] [--laps N] [--ai N] [--profile NAME]
                           [--fullscreen] [--args "name=value ..."] [NAME=VALUE ...]

Passes FM2_ARGS to the game's own developer command line (src/hooks/cmdline_args.cpp).
launch=true runs the CommandLineCircuitRace flow, which loads the track and starts the race
without the title screen or menus. --args adds any other option CForzaCommandLineParameters
reads (e.g. "nohud=true noattractvideo=true aiskill=3"); booleans take "true", not "1".
The game keeps running with FM2_DEBUG=1, so fm2ctl.py works after.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('--track', type=int, help='Tracks id (default: the game\'s own)')
ap.add_argument('--car', type=int, help='car id (default: the game\'s own)')
ap.add_argument('--laps', type=int)
ap.add_argument('--ai', type=int, help='number of AI cars')
ap.add_argument('--args', default='', help='extra raw command-line options')
ap.add_argument('--profile', default=os.environ.get('FM2_PROFILE'), help='profile name (default: the usual one)')
ap.add_argument('--fullscreen', action='store_true')
ap.add_argument('env', nargs='*', help='extra NAME=VALUE for the game')
a = ap.parse_args()

args = ['launch=true']
if a.track is not None: args.append(f'track={a.track}')
if a.car is not None: args.append(f'car={a.car}')
if a.laps is not None: args.append(f'numlaps={a.laps}')
if a.ai is not None: args.append(f'numaicars={a.ai}')
if a.args: args.append(a.args)
env = ['FM2_SKIP_INTRO=1', 'FM2_ARGS=' + ' '.join(args)]
if a.profile: env.append('FM2_PROFILE=' + a.profile)
if not a.fullscreen: env += ['FM2_FULLSCREEN=0', 'FM2_RESOLUTION=1280x720']
sys.exit(subprocess.call([sys.executable, os.path.join(HERE, 'fm2ctl.py'), 'start'] + env + a.env))
