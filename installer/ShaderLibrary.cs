// FH1Installer - builds the native renderer's shader library (fh1_shaders.nfsp) on the user's PC, from the user's
// own game files. The C# form of tools\build_shader_library.ps1 and its Python scripts:
//   1. the tracks' shader files are inside zip archives, stored with method 21 (Xbox LZX): fh1_lzx_decode.exe
//   2. the shader containers (2008 layout: 102A1100 pixel, 102A1101 vertex) are cut out of every .fxobj file and
//      out of default.xex's loaded image (written by fh1.exe --fh1_unpack_image)
//   3. each container is translated to HLSL (fh1_hlsl.exe, XenosRecomp) and compiled to SPIR-V (dxc.exe)
//   4. fh1_pack_library.exe packs containers and SPIR-V into the library
// Everything it makes is the user's own game data and stays on this PC.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Fh1Installer {

public sealed class LibraryResult {
  public int Containers;   // distinct shaders found
  public int Compiled;     // of them, translated and compiled
  public int ArchiveFiles; // shader files unpacked from the zip archives
  public int ArchiveFailed;
  public long Bytes;       // size of the library
}

public static class ShaderLibrary {
  public const string LibraryName = "fh1_shaders.nfsp";
  public static readonly string[] Tools = {
    "fh1_hlsl.exe", "fh1_lzx_decode.exe", "fh1_pack_library.exe", "shader_common.h", "dxc.exe", "dxcompiler.dll",
  };

  // `progress(stage, done, total)`: total 0 = no count for this stage. Returns null when cancelled.
  public static LibraryResult Build(string gameFolder, string toolsFolder, string imagePath, string workFolder,
                                    string libraryPath, Action<string, int, int> progress, Func<bool> cancelled) {
    foreach (string tool in Tools) {
      if (!File.Exists(Path.Combine(toolsFolder, tool))) {
        throw new FileNotFoundException("A shader tool is missing: " + tool);
      }
    }
    if (Directory.Exists(workFolder)) Directory.Delete(workFolder, true);
    string containers = Path.Combine(workFolder, "containers");
    string spirv = Path.Combine(workFolder, "spirv");
    string scratch = Path.Combine(workFolder, "scratch");
    Directory.CreateDirectory(containers);
    Directory.CreateDirectory(spirv);
    Directory.CreateDirectory(scratch);
    LibraryResult result = new LibraryResult();

    // 1 and 2: every .fxobj (loose, and inside the archives) and the image.
    progress("Looking for the game's shaders", 0, 0);
    HashSet<string> seen = new HashSet<string>();
    foreach (string path in Directory.GetFiles(gameFolder, "*.fxobj", SearchOption.AllDirectories)) {
      if (cancelled()) return null;
      Scan(File.ReadAllBytes(path), containers, seen);
    }
    string[] archives = Directory.GetFiles(gameFolder, "*.zip", SearchOption.AllDirectories);
    HashSet<string> unpacked = new HashSet<string>();
    for (int i = 0; i < archives.Length; i++) {
      if (cancelled()) return null;
      progress("Looking for the game's shaders (archives)", i, archives.Length);
      foreach (byte[] content in ShaderFilesOf(archives[i], toolsFolder, scratch, unpacked, result)) {
        Scan(content, containers, seen);
      }
    }
    if (imagePath != null && File.Exists(imagePath)) Scan(File.ReadAllBytes(imagePath), containers, seen);
    result.Containers = seen.Count;
    if (seen.Count == 0) throw new InvalidDataException("No shaders were found in the game's files.");

    // 3: translate and compile, several at a time. One process per shader: a container the translator cannot
    // read ends that process and nothing else.
    string[] names = new string[seen.Count];
    seen.CopyTo(names);
    Array.Sort(names, StringComparer.Ordinal);
    int done = 0;
    int compiled = 0;
    ParallelOptions options = new ParallelOptions();
    options.MaxDegreeOfParallelism = Math.Max(2, Environment.ProcessorCount);
    string common = Path.Combine(toolsFolder, "shader_common.h");
    Parallel.ForEach(names, options, delegate(string name, ParallelLoopState state) {
      if (cancelled()) {
        state.Stop();
        return;
      }
      if (TranslateAndCompile(name, containers, spirv, scratch, toolsFolder, common)) {
        Interlocked.Increment(ref compiled);
      }
      int now = Interlocked.Increment(ref done);
      progress("Preparing shaders", now, names.Length);
    });
    if (cancelled()) return null;
    result.Compiled = compiled;

    // 4: pack. The packer refuses to overwrite: it writes a new file, which then replaces the old library.
    progress("Writing the shader library", 0, 0);
    string fresh = libraryPath + ".new";
    if (File.Exists(fresh)) File.Delete(fresh);
    int code = Run(Path.Combine(toolsFolder, "fh1_pack_library.exe"),
                   Quote(containers) + " " + Quote(spirv) + " " + Quote(fresh), 600);
    if (code != 0 || !File.Exists(fresh)) throw new InvalidOperationException("Packing the shader library failed (code " + code + ").");
    if (File.Exists(libraryPath)) File.Delete(libraryPath);
    File.Move(fresh, libraryPath);
    result.Bytes = new FileInfo(libraryPath).Length;
    Directory.Delete(workFolder, true);
    return result;
  }

  // ---- containers ----

  static readonly byte[] PixelSignature = { 0x10, 0x2A, 0x11, 0x00 };

  static uint Be32(byte[] b, int o) {
    return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
  }

  // Writes every container found in `data` that was not seen before (tools/fh1_extract_shaders.py).
  static void Scan(byte[] data, string containers, HashSet<string> seen) {
    using (SHA1 sha = SHA1.Create()) {
      for (int o = 0; o + 36 <= data.Length; o++) {
        if (data[o] != 0x10 || data[o + 1] != 0x2A || data[o + 2] != 0x11 || data[o + 3] > 1) continue;
        char kind = data[o + 3] == PixelSignature[3] ? 'p' : 'v';
        long virtualSize = Be32(data, o + 4);
        long physicalSize = Be32(data, o + 8);
        long constants = Be32(data, o + 16), definitions = Be32(data, o + 20), shader = Be32(data, o + 24);
        long size = virtualSize + physicalSize;
        bool ok = 36 <= virtualSize && virtualSize < 0x40000 && 0 < physicalSize && physicalSize < 0x100000 &&
                  physicalSize % 4 == 0 && o + size <= data.Length && shader < virtualSize &&
                  constants < virtualSize && definitions < virtualSize;
        if (!ok) continue;
        byte[] hash = sha.ComputeHash(data, o, (int)size);
        StringBuilder name = new StringBuilder();
        name.Append(kind).Append('_');
        for (int i = 0; i < 8; i++) name.Append(hash[i].ToString("x2"));
        string text = name.ToString();
        if (!seen.Add(text)) continue;
        using (FileStream f = File.Create(Path.Combine(containers, text + ".bin"))) {
          f.Write(data, o, (int)size);
        }
      }
    }
  }

  // ---- archives (tools/fh1_unpack_archives.py) ----

  // FH1's archives have no local file headers: an entry's data starts right at the offset in the central directory.
  static IEnumerable<byte[]> ShaderFilesOf(string archive, string toolsFolder, string scratch, HashSet<string> unpacked,
                                           LibraryResult result) {
    List<byte[]> contents = new List<byte[]>();
    using (FileStream f = new FileStream(archive, FileMode.Open, FileAccess.Read, FileShare.Read)) {
      // End of central directory: in the last 64 KB (+ comment).
      int tailLength = (int)Math.Min(f.Length, 65536 + 22);
      byte[] tail = new byte[tailLength];
      f.Position = f.Length - tailLength;
      ReadFully(f, tail, tailLength);
      int end = -1;
      for (int i = tailLength - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) { end = i; break; }
      }
      if (end < 0) return contents;  // not a zip
      uint directorySize = BitConverter.ToUInt32(tail, end + 12);
      uint directoryOffset = BitConverter.ToUInt32(tail, end + 16);
      if (directoryOffset == 0xFFFFFFFF || directoryOffset + (long)directorySize > f.Length) {
        return contents;  // zip64 or damaged: the game has none of these
      }
      byte[] directory = new byte[directorySize];
      f.Position = directoryOffset;
      ReadFully(f, directory, directory.Length);
      // The entry count of the end record is not used: it has 16 bits, and the tracks' archive holds 230,057
      // files (the count wraps round to 33,449). The directory is read to its end.
      int o = 0;
      while (o + 46 <= directory.Length) {
        if (BitConverter.ToUInt32(directory, o) != 0x02014B50) break;
        int method = BitConverter.ToUInt16(directory, o + 10);
        uint crc = BitConverter.ToUInt32(directory, o + 16);
        uint packedSize = BitConverter.ToUInt32(directory, o + 20);
        uint fullSize = BitConverter.ToUInt32(directory, o + 24);
        int nameLength = BitConverter.ToUInt16(directory, o + 28);
        int extraLength = BitConverter.ToUInt16(directory, o + 30);
        int commentLength = BitConverter.ToUInt16(directory, o + 32);
        uint offset = BitConverter.ToUInt32(directory, o + 42);
        string name = Encoding.ASCII.GetString(directory, o + 46, Math.Min(nameLength, directory.Length - o - 46));
        o += 46 + nameLength + extraLength + commentLength;
        if (!name.EndsWith(".fxobj", StringComparison.OrdinalIgnoreCase)) continue;
        // The same file sits in many archives: once is enough.
        if (!unpacked.Add(name.Replace('\\', '/').ToLowerInvariant() + ":" + crc.ToString("x8"))) continue;
        if (offset + (long)packedSize > f.Length || packedSize > 64 * 1024 * 1024 || fullSize > 64 * 1024 * 1024) {
          result.ArchiveFailed++;
          continue;
        }
        byte[] packed = new byte[packedSize];
        f.Position = offset;
        ReadFully(f, packed, packed.Length);
        byte[] content = null;
        if (method == 0) {
          content = packed;
        } else if (method == 21) {
          content = DecodeLzx(packed, (int)fullSize, toolsFolder, scratch);
        }
        if (content == null || Crc32(content) != crc) {
          result.ArchiveFailed++;
          continue;
        }
        result.ArchiveFiles++;
        contents.Add(content);
      }
    }
    return contents;
  }

  static void ReadFully(Stream s, byte[] buffer, int length) {
    int done = 0;
    while (done < length) {
      int n = s.Read(buffer, done, length - done);
      if (n <= 0) throw new EndOfStreamException("Unexpected end of an archive.");
      done += n;
    }
  }

  // A method-21 entry is a series of frames: 0xFF, 16-bit uncompressed size, 16-bit block size (or only a 16-bit
  // block size for a full 32 KB frame), big-endian, then the LZX block. The decoder takes the blocks alone.
  static byte[] DecodeLzx(byte[] data, int fullSize, string toolsFolder, string scratch) {
    MemoryStream blocks = new MemoryStream();
    int pos = 0, produced = 0;
    while (produced < fullSize && pos + 2 <= data.Length) {
      int frame, block;
      if (data[pos] == 0xFF) {
        if (pos + 5 > data.Length) break;
        frame = data[pos + 1] << 8 | data[pos + 2];
        block = data[pos + 3] << 8 | data[pos + 4];
        pos += 5;
      } else {
        frame = 32768;
        block = data[pos] << 8 | data[pos + 1];
        pos += 2;
      }
      if (block == 0 || pos + block > data.Length) break;
      blocks.Write(data, pos, block);
      pos += block;
      produced += frame;
    }
    string stem = Path.Combine(scratch, "lzx_" + Guid.NewGuid().ToString("N"));
    string input = stem + ".in", output = stem + ".out";
    try {
      File.WriteAllBytes(input, blocks.ToArray());
      int code = Run(Path.Combine(toolsFolder, "fh1_lzx_decode.exe"),
                     Quote(input) + " " + Quote(output) + " " + fullSize, 60);
      if (code != 0 || !File.Exists(output)) return null;
      return File.ReadAllBytes(output);
    } finally {
      if (File.Exists(input)) File.Delete(input);
      if (File.Exists(output)) File.Delete(output);
    }
  }

  static uint[] crcTable_;
  static uint Crc32(byte[] data) {
    if (crcTable_ == null) {
      uint[] table = new uint[256];
      for (uint i = 0; i < 256; i++) {
        uint c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        table[i] = c;
      }
      crcTable_ = table;
    }
    uint crc = 0xFFFFFFFF;
    for (int i = 0; i < data.Length; i++) crc = crcTable_[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
  }

  // ---- translation and compilation (tools/fh1_translate_shaders.py, fh1_compile_shaders.py) ----

  static bool TranslateAndCompile(string name, string containers, string spirv, string scratch, string toolsFolder,
                                  string common) {
    string input = Path.Combine(scratch, name + "_in");
    string output = Path.Combine(scratch, name + "_out");
    try {
      Directory.CreateDirectory(input);
      File.Copy(Path.Combine(containers, name + ".bin"), Path.Combine(input, name + ".bin"), true);
      // A damaged container can make the translator run away: stopped after 60 s.
      Run(Path.Combine(toolsFolder, "fh1_hlsl.exe"), Quote(input) + " " + Quote(output) + " " + Quote(common), 60);
      string hlsl = Path.Combine(output, name + ".hlsl");
      if (!File.Exists(hlsl)) return false;
      // Vertex shaders: SV_VertexID is the index of the draw, as on the console (-fvk-support-nonzero-base-vertex).
      string target = name[0] == 'p' ? "ps_6_6" : "vs_6_6 -fvk-invert-y -fvk-support-nonzero-base-vertex";
      string spv = Path.Combine(spirv, name + ".spv");
      int code = Run(Path.Combine(toolsFolder, "dxc.exe"),
                     "-spirv -T " + target + " -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Fo " +
                         Quote(spv) + " " + Quote(hlsl),
                     300);
      if (code != 0 && File.Exists(spv)) File.Delete(spv);
      return code == 0 && File.Exists(spv);
    } catch (IOException) {
      return false;
    } finally {
      try {
        if (Directory.Exists(input)) Directory.Delete(input, true);
        if (Directory.Exists(output)) Directory.Delete(output, true);
      } catch (IOException) {
      }
    }
  }

  static string Quote(string path) { return "\"" + path + "\""; }

  // Runs a tool without a window; -1 when it had to be stopped after `seconds`.
  public static int Run(string exe, string arguments, int seconds) {
    ProcessStartInfo info = new ProcessStartInfo(exe, arguments);
    info.UseShellExecute = false;
    info.CreateNoWindow = true;
    info.WorkingDirectory = Path.GetDirectoryName(exe);
    using (Process p = Process.Start(info)) {
      if (!p.WaitForExit(seconds * 1000)) {
        try { p.Kill(); } catch (InvalidOperationException) { }
        p.WaitForExit(5000);
        return -1;
      }
      return p.ExitCode;
    }
  }
}

}  // namespace Fh1Installer
