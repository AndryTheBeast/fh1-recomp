// FH1Installer - reads an Xbox 360 disc image (.iso: XDVDFS, XGD2 / XGD3). The C# form of tools/extract_xiso.ps1.
// Nothing is sent anywhere: it reads the image on this computer.
// Written for the C# 5 compiler that is part of Windows (.NET Framework 4.8): no newer language features.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Fh1Installer {

public sealed class DiscFile {
  public string Path;     // relative, with backslashes
  public long Offset;     // in the image
  public long Size;
}

public sealed class XDisc : IDisposable {
  const int SectorSize = 2048;
  static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

  readonly FileStream stream_;
  public readonly List<DiscFile> Files = new List<DiscFile>();
  public readonly List<string> Folders = new List<string>();
  public long TotalBytes;

  public XDisc(string isoPath) {
    stream_ = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read);
    try {
      ReadTree();
    } catch {
      stream_.Dispose();
      throw;
    }
  }

  public void Dispose() { stream_.Dispose(); }

  byte[] ReadAt(long position, int length) {
    if (position < 0 || position + length > stream_.Length) {
      throw new InvalidDataException("The disc image is shorter than its own file table says (a cut or damaged .iso).");
    }
    byte[] data = new byte[length];
    stream_.Position = position;
    int done = 0;
    while (done < length) {
      int n = stream_.Read(data, done, length - done);
      if (n <= 0) throw new EndOfStreamException("Could not read the disc image.");
      done += n;
    }
    return data;
  }

  bool HasMagic(long position) {
    if (position + Magic.Length > stream_.Length) return false;
    byte[] b = ReadAt(position, Magic.Length);
    for (int i = 0; i < Magic.Length; i++) {
      if (b[i] != Magic[i]) return false;
    }
    return true;
  }

  void ReadTree() {
    // Where the game partition starts: a plain XISO, XGD3, XGD2, XGD1.
    long[] starts = { 0, 0x2080000, 0xFD90000, 0x18300000 };
    long partition = -1;
    foreach (long start in starts) {
      if (HasMagic(start + 32L * SectorSize)) { partition = start; break; }
    }
    if (partition < 0) {
      throw new InvalidDataException("This file is not an Xbox 360 disc image (no XDVDFS volume in it).");
    }
    byte[] volume = ReadAt(partition + 32L * SectorSize + 20, 8);
    uint rootSector = BitConverter.ToUInt32(volume, 0);
    uint rootSize = BitConverter.ToUInt32(volume, 4);

    // Each directory is a binary tree of entries; offsets are in 4-byte units from the start of the directory.
    Stack<object[]> dirs = new Stack<object[]>();
    dirs.Push(new object[] { rootSector, rootSize, "" });
    while (dirs.Count > 0) {
      object[] d = dirs.Pop();
      string parent = (string)d[2];
      byte[] data = ReadAt(partition + (long)(uint)d[0] * SectorSize, checked((int)(uint)d[1]));
      Stack<int> nodes = new Stack<int>();
      HashSet<int> seen = new HashSet<int>();
      nodes.Push(0);
      while (nodes.Count > 0) {
        int o = nodes.Pop();
        if (seen.Contains(o) || o + 14 > data.Length) continue;
        seen.Add(o);
        ushort left = BitConverter.ToUInt16(data, o);
        if (left == 0xFFFF) continue;
        ushort right = BitConverter.ToUInt16(data, o + 2);
        uint sector = BitConverter.ToUInt32(data, o + 4);
        uint size = BitConverter.ToUInt32(data, o + 8);
        byte attributes = data[o + 12];
        int nameLength = data[o + 13];
        if (o + 14 + nameLength > data.Length) continue;
        string name = Encoding.ASCII.GetString(data, o + 14, nameLength);
        if (left != 0) nodes.Push(left * 4);
        if (right != 0) nodes.Push(right * 4);
        // A name from the disc never leaves the chosen folder.
        if (name.Length == 0 || name == "." || name == ".." || name.IndexOfAny(new[] { '\\', '/', ':' }) >= 0) continue;
        string rel = parent.Length > 0 ? parent + "\\" + name : name;
        if ((attributes & 0x10) != 0) {
          Folders.Add(rel);
          if (size > 0) dirs.Push(new object[] { sector, size, rel });
        } else {
          DiscFile f = new DiscFile();
          f.Path = rel;
          f.Offset = partition + (long)sector * SectorSize;
          f.Size = size;
          if (f.Offset + f.Size > stream_.Length) {
            throw new InvalidDataException("The disc image is cut short: '" + rel + "' ends after the end of the .iso.");
          }
          Files.Add(f);
          TotalBytes += size;
        }
      }
    }
  }

  public DiscFile Find(string relativePath) {
    foreach (DiscFile f in Files) {
      if (string.Equals(f.Path, relativePath, StringComparison.OrdinalIgnoreCase)) return f;
    }
    return null;
  }

  public byte[] ReadStart(DiscFile file, int bytes) {
    return ReadAt(file.Offset, (int)Math.Min(bytes, file.Size));
  }

  // Copies every file of the disc into `root`. A file that is already there with its full size is kept (an
  // installation that was cancelled goes on where it stopped); a file being written has the name *.part until it
  // is complete. `progress` gets the bytes done so far and the file in hand; `cancelled` is asked between blocks.
  // Returns false when cancelled.
  public bool Extract(string root, Action<long, string> progress, Func<bool> cancelled) {
    Directory.CreateDirectory(root);
    foreach (string folder in Folders) {
      Directory.CreateDirectory(System.IO.Path.Combine(root, folder));
    }
    byte[] buffer = new byte[4 * 1024 * 1024];
    long done = 0;
    foreach (DiscFile file in Files) {
      if (cancelled()) return false;
      string target = System.IO.Path.Combine(root, file.Path);
      Directory.CreateDirectory(System.IO.Path.GetDirectoryName(target));
      progress(done, file.Path);
      FileInfo present = new FileInfo(target);
      if (present.Exists && present.Length == file.Size) {
        done += file.Size;
        continue;
      }
      string part = target + ".part";
      using (FileStream output = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None)) {
        stream_.Position = file.Offset;
        long left = file.Size;
        while (left > 0) {
          if (cancelled()) {
            output.Close();
            File.Delete(part);
            return false;
          }
          int n = stream_.Read(buffer, 0, (int)Math.Min(buffer.Length, left));
          if (n <= 0) throw new EndOfStreamException("Could not read '" + file.Path + "' from the disc image.");
          output.Write(buffer, 0, n);
          left -= n;
          done += n;
          progress(done, file.Path);
        }
      }
      if (File.Exists(target)) File.Delete(target);
      File.Move(part, target);
    }
    progress(done, "");
    return true;
  }
}

// What a .xex says about itself in its (unencrypted) header.
public sealed class XexInfo {
  public uint TitleId;
  public uint MediaId;  // differs between the discs of one game (regions, re-releases)
  public uint Version;  // major 4 bits, minor 4 bits, build 16 bits, revision 8 bits

  public string VersionText {
    get {
      return (Version >> 28) + "." + ((Version >> 24) & 0xF) + "." + ((Version >> 8) & 0xFFFF) + "." + (Version & 0xFF);
    }
  }

  static uint Be32(byte[] b, int o) {
    return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
  }

  // Null when the bytes are not the start of a XEX2 file or hold no execution info.
  public static XexInfo Read(byte[] head) {
    if (head.Length < 0x18 || head[0] != 'X' || head[1] != 'E' || head[2] != 'X' || head[3] != '2') return null;
    uint count = Be32(head, 0x14);
    for (uint i = 0; i < count; i++) {
      int entry = 0x18 + (int)i * 8;
      if (entry + 8 > head.Length) return null;
      if (Be32(head, entry) != 0x00040006) continue;  // execution info
      int o = (int)Be32(head, entry + 4);
      if (o < 0 || o + 16 > head.Length) return null;
      XexInfo info = new XexInfo();
      info.MediaId = Be32(head, o);
      info.Version = Be32(head, o + 4);
      info.TitleId = Be32(head, o + 12);
      return info;
    }
    return null;
  }
}

}  // namespace Fh1Installer
