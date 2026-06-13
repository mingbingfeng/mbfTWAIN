using System;
using System.Collections.Generic;
using System.Drawing;

namespace MbfTwain.VirtualScannerConfig;

internal sealed class ThumbnailCache(int limit) : IDisposable
{
    private readonly object sync = new();
    private readonly Dictionary<ThumbnailCacheKey, Bitmap> cache = [];
    private readonly Queue<ThumbnailCacheKey> cacheOrder = [];

    public Image? TryClone(ThumbnailCacheKey cacheKey)
    {
        lock (sync)
        {
            return cache.TryGetValue(cacheKey, out Bitmap? cached)
                ? new Bitmap(cached)
                : null;
        }
    }

    public void Store(ThumbnailCacheKey cacheKey, Bitmap thumbnail)
    {
        lock (sync)
        {
            if (cache.ContainsKey(cacheKey))
            {
                thumbnail.Dispose();
                return;
            }

            cache.Add(cacheKey, thumbnail);
            cacheOrder.Enqueue(cacheKey);
            while (cache.Count > limit && cacheOrder.Count > 0)
            {
                ThumbnailCacheKey oldestKey = cacheOrder.Dequeue();
                if (cache.Remove(oldestKey, out Bitmap? oldestThumbnail))
                {
                    oldestThumbnail.Dispose();
                }
            }
        }
    }

    public void Dispose()
    {
        lock (sync)
        {
            foreach (Bitmap thumbnail in cache.Values)
            {
                thumbnail.Dispose();
            }

            cache.Clear();
            cacheOrder.Clear();
        }
    }
}

internal readonly record struct ThumbnailCacheKey(
    string Path,
    long LastWriteTimeUtcTicks,
    long Length,
    int RotationDegrees,
    int Width,
    int Height);
