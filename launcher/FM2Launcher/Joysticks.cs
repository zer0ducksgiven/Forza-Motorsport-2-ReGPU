using System.Runtime.InteropServices;
using System.Text;

namespace FM2Launcher;

/// <summary>
/// Wheels, pedals, shifters and button boxes through SDL3, for the Bindings window: the same SDL3.dll and
/// hints as the game (src/input/wheel.cpp), so devices have the same GUIDs the bindings name them by.
/// </summary>
public static class Joysticks
{
    const string Lib = "SDL3";
    const uint InitJoystick = 0x200;

    // SDL3.dll ships with the game (build\tu), not next to the launcher. Linux: the system's SDL3 (fm2.exe's
    // SDL3.dll runs inside Wine, out of the launcher's reach).
    static string LibraryFile => Platform.Linux ? "libSDL3.so.0" : Path.Combine(Path.GetDirectoryName(Paths.Exe)!, "SDL3.dll");

    static Joysticks()
    {
        NativeLibrary.SetDllImportResolver(typeof(Joysticks).Assembly, (name, _, _) =>
            name == Lib && NativeLibrary.TryLoad(LibraryFile, out var h) ? h : IntPtr.Zero);
    }

    [StructLayout(LayoutKind.Sequential)] struct Guid16 { public ulong A, B; }

    [DllImport(Lib)][return: MarshalAs(UnmanagedType.U1)] static extern bool SDL_Init(uint flags);
    [DllImport(Lib)][return: MarshalAs(UnmanagedType.U1)] static extern bool SDL_SetHint([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Lib)] static extern IntPtr SDL_GetJoysticks(out int count);
    [DllImport(Lib)] static extern void SDL_free(IntPtr p);
    [DllImport(Lib)] static extern IntPtr SDL_OpenJoystick(uint id);
    [DllImport(Lib)] static extern void SDL_CloseJoystick(IntPtr j);
    [DllImport(Lib)] static extern IntPtr SDL_GetJoystickName(IntPtr j);
    [DllImport(Lib)] static extern Guid16 SDL_GetJoystickGUID(IntPtr j);
    [DllImport(Lib)] static extern void SDL_GUIDToString(Guid16 guid, byte[] buf, int len);
    [DllImport(Lib)] static extern int SDL_GetJoystickType(IntPtr j);
    [DllImport(Lib)] static extern ushort SDL_GetJoystickVendor(IntPtr j);
    [DllImport(Lib)] static extern ushort SDL_GetJoystickProduct(IntPtr j);
    [DllImport(Lib)] static extern int SDL_GetNumJoystickAxes(IntPtr j);
    [DllImport(Lib)] static extern int SDL_GetNumJoystickButtons(IntPtr j);
    [DllImport(Lib)] static extern int SDL_GetNumJoystickHats(IntPtr j);
    [DllImport(Lib)] static extern short SDL_GetJoystickAxis(IntPtr j, int axis);
    [DllImport(Lib)][return: MarshalAs(UnmanagedType.U1)] static extern bool SDL_GetJoystickAxisInitialState(IntPtr j, int axis, out short state);
    [DllImport(Lib)][return: MarshalAs(UnmanagedType.U1)] static extern bool SDL_GetJoystickButton(IntPtr j, int button);
    [DllImport(Lib)] static extern byte SDL_GetJoystickHat(IntPtr j, int hat);
    [DllImport(Lib)] static extern void SDL_UpdateJoysticks();

    public sealed class Device
    {
        internal IntPtr Handle;
        internal uint Id;
        public string Guid { get; init; } = "";
        public string Name { get; init; } = "";
        public string Kind { get; init; } = "";
        public int Axes { get; init; }
        public int Buttons { get; init; }
        public int Hats { get; init; }
        public short[] Rest { get; init; } = [];
    }

    public sealed record State(string Guid, short[] Axes, bool[] Buttons, byte[] Hats);

    static readonly object Gate = new();
    static bool _started;
    static readonly List<Device> Open = [];

    static bool Start()
    {
        if (_started) return true;
        if (Platform.Linux ? !NativeLibrary.TryLoad(LibraryFile, out _) : !File.Exists(LibraryFile)) return false;
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI", "0");
        SDL_SetHint("SDL_JOYSTICK_RAWINPUT", "0");
        SDL_SetHint("SDL_XINPUT_ENABLED", "0");
        return _started = SDL_Init(InitJoystick);
    }

    static string KindOf(int type) => type switch
    {
        2 => "wheel",
        3 => "arcade stick",
        4 => "flight stick",
        5 => "dance pad",
        6 => "guitar",
        7 => "drum kit",
        8 => "arcade pad",
        9 => "pedals / throttle",
        1 => "gamepad",
        _ => "device",
    };

    /// <summary>The connected devices (opening new ones, closing those gone).</summary>
    public static List<Device> List()
    {
        lock (Gate)
        {
            if (!Start()) return [];
            SDL_UpdateJoysticks();
            var ptr = SDL_GetJoysticks(out int n);
            var ids = new uint[Math.Max(n, 0)];
            for (int i = 0; i < ids.Length; i++) ids[i] = (uint)Marshal.ReadInt32(ptr, i * 4);
            if (ptr != IntPtr.Zero) SDL_free(ptr);
            foreach (var gone in Open.Where(d => !ids.Contains(d.Id)).ToList())
            {
                SDL_CloseJoystick(gone.Handle);
                Open.Remove(gone);
            }
            foreach (var id in ids.Where(id => Open.All(d => d.Id != id)))
            {
                var j = SDL_OpenJoystick(id);
                if (j == IntPtr.Zero) continue;
                var buf = new byte[64];
                SDL_GUIDToString(SDL_GetJoystickGUID(j), buf, buf.Length);
                int axes = Math.Max(0, SDL_GetNumJoystickAxes(j));
                var rest = new short[axes];
                for (int a = 0; a < axes; a++)
                    if (!SDL_GetJoystickAxisInitialState(j, a, out rest[a])) rest[a] = SDL_GetJoystickAxis(j, a);
                Open.Add(new Device
                {
                    Handle = j,
                    Id = id,
                    Guid = Encoding.ASCII.GetString(buf).TrimEnd('\0'),
                    Name = Marshal.PtrToStringUTF8(SDL_GetJoystickName(j)) ?? "?",
                    Kind = KindOf(SDL_GetJoystickType(j)),
                    Axes = axes,
                    Buttons = Math.Max(0, SDL_GetNumJoystickButtons(j)),
                    Hats = Math.Max(0, SDL_GetNumJoystickHats(j)),
                    Rest = rest,
                });
            }
            return [.. Open];
        }
    }

    /// <summary>Every open device's axes, buttons and hats now.</summary>
    public static List<State> Read()
    {
        lock (Gate)
        {
            if (!_started) return [];
            SDL_UpdateJoysticks();
            var states = new List<State>();
            foreach (var d in Open)
            {
                var axes = new short[d.Axes];
                for (int i = 0; i < d.Axes; i++) axes[i] = SDL_GetJoystickAxis(d.Handle, i);
                var buttons = new bool[d.Buttons];
                for (int i = 0; i < d.Buttons; i++) buttons[i] = SDL_GetJoystickButton(d.Handle, i);
                var hats = new byte[d.Hats];
                for (int i = 0; i < d.Hats; i++) hats[i] = SDL_GetJoystickHat(d.Handle, i);
                states.Add(new State(d.Guid, axes, buttons, hats));
            }
            return states;
        }
    }
}
