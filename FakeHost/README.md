# FakeHost

`FakeHost` is an isolated analysis host for observing DLL behavior inside a sacrificial x64 process.

It does not inject into other processes. It either waits for an external injector to target this process, or optionally loads a DLL into itself with `LoadLibraryW`.

## Build

```powershell
MSBuild.exe FakeHost\FakeHost.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Output:

```text
FakeHost\x64\Release\FakeHost.exe
```

## Usage

Wait for an external injector:

```powershell
FakeHost\x64\Release\FakeHost.exe --title "Fake Analysis Host" --dump-dir dumps
```

Load a DLL directly into the host process:

```powershell
FakeHost\x64\Release\FakeHost.exe --load D:\payload.dll --dump-dir dumps
```

Use a specific window class/title if the sample searches for a window:

```powershell
FakeHost\x64\Release\FakeHost.exe --class SomeWindowClass --title "Some Window Title" --dump-dir dumps
```

Dump every 10 seconds:

```powershell
FakeHost\x64\Release\FakeHost.exe --interval 10 --dump-dir dumps
```

By default, dumps include readable executable regions only. Add `--all` to dump all readable committed regions:

```powershell
FakeHost\x64\Release\FakeHost.exe --all --dump-dir dumps
```

## Runtime Keys

- `d`: dump memory now
- `m`: write module list now
- `q`: quit

Each dump creates a timestamped folder containing:

- `modules.tsv`: loaded module list
- `regions.tsv`: dumped memory region metadata
- `region_*.bin`: raw memory regions, named with base address, size, region type, and protection

Load `region_*.bin` into IDA as raw binary using the base address from `regions.tsv`.
