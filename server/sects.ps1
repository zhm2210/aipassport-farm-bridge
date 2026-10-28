# sects.ps1 — 从运行中的 Bambu Farm Manager Client 进程内存提取新鲜的 x-bbl-sec-ts 值和 JWT
# 用法: powershell -File sects.ps1  →  输出 JSON: [{"val":"...","path":"..."}, ...]
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;

public class MemReader {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, int dwSize, out IntPtr lpNumberOfBytesRead);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern int VirtualQueryEx(IntPtr hProcess, IntPtr lpAddress, out MEMORY_BASIC_INFORMATION lpBuffer, int dwLength);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [StructLayout(LayoutKind.Sequential)]
    public struct MEMORY_BASIC_INFORMATION {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public IntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

    const uint MEM_COMMIT = 0x1000;
    const uint PAGE_NOACCESS = 0x01;
    const uint PAGE_GUARD = 0x100;
    const uint PROCESS_QUERY_INFORMATION = 0x0400;
    const uint PROCESS_VM_READ = 0x0010;

    public static List<string> Scan(int pid, byte[] needle, int maxResults, int maxScanMB) {
        var results = new List<string>();
        IntPtr h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
        if (h == IntPtr.Zero) return results;
        try {
            IntPtr addr = IntPtr.Zero;
            MEMORY_BASIC_INFORMATION mbi;
            long scanned = 0;
            long limit = (long)maxScanMB * 1024 * 1024;
            int chunkSize = 4 * 1024 * 1024;
            byte[] carry = new byte[0];
            while (scanned < limit && VirtualQueryEx(h, addr, out mbi, Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION))) != 0) {
                long regionSize = mbi.RegionSize.ToInt64();
                if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_NOACCESS) == 0 && (mbi.Protect & PAGE_GUARD) == 0
                    && mbi.Type != 0 /*MEM_IMAGE optional - include all commit*/) {
                    long read = 0;
                    while (read < regionSize && scanned < limit) {
                        int toRead = (int)Math.Min(chunkSize, regionSize - read);
                        byte[] buf = new byte[toRead];
                        IntPtr got;
                        if (!ReadProcessMemory(h, new IntPtr(mbi.BaseAddress.ToInt64() + read), buf, toRead, out got) || got.ToInt64() == 0) break;
                        // search needle in carry+buf
                        byte[] combined = new byte[carry.Length + toRead];
                        Buffer.BlockCopy(carry, 0, combined, 0, carry.Length);
                        Buffer.BlockCopy(buf, 0, combined, carry.Length, toRead);
                        int idx = 0;
                        while (results.Count < maxResults) {
                            idx = IndexOf(combined, needle, idx);
                            if (idx < 0) break;
                            int start = idx + needle.Length;
                            int end = start;
                            while (end < combined.Length && IsB64(combined[end])) end++;
                            if (end - start >= 40) {
                                results.Add(Encoding.ASCII.GetString(combined, start, end - start));
                            }
                            idx += 1;
                        }
                        // keep tail overlap (needle may span chunks)
                        int keep = Math.Min(needle.Length + 64, combined.Length);
                        carry = new byte[keep];
                        Buffer.BlockCopy(combined, combined.Length - keep, carry, 0, keep);
                        read += toRead;
                        scanned += toRead;
                    }
                }
                addr = new IntPtr(mbi.BaseAddress.ToInt64() + regionSize);
                if (addr.ToInt64() <= 0) break;
            }
        } finally {
            CloseHandle(h);
        }
        return results;
    }

    static bool IsB64(byte b) {
        return (b >= (byte)'A' && b <= (byte)'Z') || (b >= (byte)'a' && b <= (byte)'z') || (b >= (byte)'0' && b <= (byte)'9') || b == (byte)'+' || b == (byte)'/' || b == (byte)'=';
    }

    static int IndexOf(byte[] hay, byte[] nee, int from) {
        for (int i = from; i <= hay.Length - nee.Length; i++) {
            bool ok = true;
            for (int j = 0; j < nee.Length; j++) { if (hay[i + j] != nee[j]) { ok = false; break; } }
            if (ok) return i;
        }
        return -1;
    }

    public static List<string> FindJwts(int pid) {
        // JWT: eyJhbGciOi...（完整三段，含点号分隔符；从 needle 本身开始捕获）
        var results = new List<string>();
        IntPtr h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
        if (h == IntPtr.Zero) return results;
        try {
            byte[] head = Encoding.ASCII.GetBytes("eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9");
            IntPtr addr = IntPtr.Zero;
            MEMORY_BASIC_INFORMATION mbi;
            long scanned = 0;
            long limit = 2048L * 1024 * 1024;
            int chunkSize = 4 * 1024 * 1024;
            byte[] carry = new byte[0];
            while (scanned < limit && VirtualQueryEx(h, addr, out mbi, Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION))) != 0) {
                long regionSize = mbi.RegionSize.ToInt64();
                if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_NOACCESS) == 0 && (mbi.Protect & PAGE_GUARD) == 0) {
                    long read = 0;
                    while (read < regionSize && scanned < limit) {
                        int toRead = (int)Math.Min(chunkSize, regionSize - read);
                        byte[] buf = new byte[toRead];
                        IntPtr got;
                        if (!ReadProcessMemory(h, new IntPtr(mbi.BaseAddress.ToInt64() + read), buf, toRead, out got) || got.ToInt64() == 0) break;
                        byte[] combined = new byte[carry.Length + toRead];
                        Buffer.BlockCopy(carry, 0, combined, 0, carry.Length);
                        Buffer.BlockCopy(buf, 0, combined, carry.Length, toRead);
                        int idx = 0;
                        while (results.Count < 20) {
                            idx = IndexOf(combined, head, idx);
                            if (idx < 0) break;
                            // 从 needle 起点开始，读取 base64url + '.' 字符
                            int end = idx;
                            while (end < combined.Length && (IsB64(combined[end]) || combined[end] == (byte)'.' || combined[end] == (byte)'-' || combined[end] == (byte)'_')) end++;
                            if (end - idx >= 500) {
                                results.Add(Encoding.ASCII.GetString(combined, idx, end - idx));
                            }
                            idx += 1;
                        }
                        int keep = Math.Min(head.Length + 64, combined.Length);
                        carry = new byte[keep];
                        Buffer.BlockCopy(combined, combined.Length - keep, carry, 0, keep);
                        read += toRead;
                        scanned += toRead;
                    }
                }
                addr = new IntPtr(mbi.BaseAddress.ToInt64() + regionSize);
                if (addr.ToInt64() <= 0) break;
            }
        } finally {
            CloseHandle(h);
        }
        return results;
    }
}
'@

# 找主进程：有窗口标题的 Bambu Farm Manager Client；否则工作集最大
$targetPid = $null
$main = Get-Process -Name '*Bambu Farm Manager Client*' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne '' } | Sort-Object WorkingSet64 -Descending | Select-Object -First 1
if ($main) { $targetPid = $main.Id }
else {
$main2 = Get-Process -Name '*Bambu Farm Manager Client*' -ErrorAction SilentlyContinue | Sort-Object WorkingSet64 -Descending | Select-Object -First 1
if ($main2) { $targetPid = $main2.Id }
else {
$conns = Get-NetTCPConnection -RemotePort 8888 -State Established -ErrorAction SilentlyContinue
if ($conns) {
    $pids = $conns | Select-Object -ExpandProperty OwningProcess -Unique
    foreach ($p in $pids) {
        $proc = Get-Process -Id $p -ErrorAction SilentlyContinue
        if ($proc -and $proc.ProcessName -like '*Bambu Farm Manager*') { $targetPid = $p; break }
    }
}
}
}
if (-not $targetPid) {
    Write-Output '{"error":"client process not found"}'
    exit 1
}

$sects = [MemReader]::Scan($targetPid, [Text.Encoding]::ASCII.GetBytes("x-bbl-sec-ts: "), 40, 1024)
$jwts = [MemReader]::FindJwts($targetPid)
$uniqJwts = @($jwts | Select-Object -Unique | Where-Object { $_.Length -ge 500 })

# 去重并按长度过滤
$uniq = @($sects | Select-Object -Unique | Where-Object { $_.Length -ge 40 })

$out = @()
foreach ($v in $uniq) {
    $out += @{ val = $v }
}
$result = @{
    pid = $targetPid
    count = $out.Count
    sects = $out
    jwts = $uniqJwts
}
$result | ConvertTo-Json -Depth 3 -Compress
