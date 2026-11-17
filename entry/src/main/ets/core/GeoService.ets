// ★ 地理位置→地名 解析服务（专门源文件）：
//   1) 系统逆地理 geoLocationManager.getAddressesFromLocation —— 免费、无需 Key，优先
//   2) 高德 Web 服务 regeo —— 系统失败时兜底，需补充下方 AMAP_WEB_KEY
//   3) 百度 Web 服务 reverse_geocoding —— 再兜底，需补充下方 BAIDU_AK
//   坐标系说明：系统定位为 WGS-84；高德期望 GCJ-02、百度可指定 coordtype=wgs84ll，
//   城市级地名对偏移不敏感，直接传坐标即可
import { geoLocationManager } from '@kit.LocationKit';
import { http } from '@kit.NetworkKit';
import { abilityAccessCtrl, common } from '@kit.AbilityKit';

// ══════════════════ 地图 API Key 调参区（自行补充） ══════════════════
// 高德开放平台 Web服务 Key（控制台创建"Web服务"类型）：https://console.amap.com
export const AMAP_WEB_KEY = '';
// 百度地图开放平台 应用 AK（启用"逆地理编码"服务）：https://lbsyun.baidu.com
export const BAIDU_AK = '';
// ═════════════════════════════════════════════════════════════════════

// ★ JSON 响应模型（只取需要的字段，JSON.parse 后一次性转成命名类型）
interface AmapRegeoResp {
  status: string;
  regeocode?: AmapRegeo;
}
interface AmapRegeo {
  formatted_address?: string;
}
interface BaiduRegeoResp {
  status?: string;
  result?: BaiduRegeoResult;
}
interface BaiduRegeoResult {
  formatted_address?: string;
}

// ★ 位置解析结果：name 可为空（逆地理失败时仅返回经纬度）
export interface GeoPlace {
  name: string;
  lat: number;
  lon: number;
}

export class GeoService {
  // 结果缓存：60s 内复用，避免频繁定位/请求
  private static cachedPlace: GeoPlace | null = null;
  private static cachedMs: number = 0;
  private static permGranted: boolean = false;
  private static permRequested: boolean = false;
  private static failReason: string = '';

  // ★ 权限：LOCATION（精确）+ APPROXIMATELY_LOCATION（模糊）必须成对申请
  static async ensurePermission(ctx: common.UIAbilityContext): Promise<boolean> {
    try {
      const atManager = abilityAccessCtrl.createAtManager();
      const result = await atManager.requestPermissionsFromUser(ctx,
        ['ohos.permission.LOCATION', 'ohos.permission.APPROXIMATELY_LOCATION']);
      let allGranted = true;
      for (let i = 0; i < result.authResults.length; i++) {
        if (result.authResults[i] !== 0) allGranted = false;
      }
      GeoService.permRequested = true;
      return allGranted;
    } catch (e) {
      console.error(`[GeoService] requestPermission fail: ${JSON.stringify(e)}`);
      return false;
    }
  }

  // ★ 主入口：当前位置 → 地名+经纬度。定位成功即返回（逆地理失败 name 为空，
  //   经纬度仍可用）；完全失败返回 null（failReason 说明原因）
  static async getPlace(ctx: common.UIAbilityContext): Promise<GeoPlace | null> {
    const now = Date.now();
    if (GeoService.cachedPlace !== null && now - GeoService.cachedMs < 60000) {
      return GeoService.cachedPlace;
    }
    GeoService.failReason = '';
    // 定位开关未开直接跳过，避免无谓等待
    try {
      if (!geoLocationManager.isLocationEnabled()) {
        GeoService.failReason = 'disabled';
        return null;
      }
    } catch (_) {
      GeoService.failReason = 'disabled';
      return null;
    }
    if (!GeoService.permGranted) {
      if (GeoService.permRequested) {
        // 之前申请被拒过：不反复弹窗打扰，直接按无权限处理
        GeoService.failReason = 'permission';
        return null;
      }
      const granted = await GeoService.ensurePermission(ctx);
      GeoService.permRequested = true;
      if (!granted) {
        GeoService.failReason = 'permission';
        return null;
      }
      GeoService.permGranted = true;
    }
    // 取当前位置（低功耗优先，地名场景无需高精度）
    let lat = 0;
    let lon = 0;
    try {
      const loc = await geoLocationManager.getCurrentLocation({
        priority: geoLocationManager.LocationRequestPriority.LOW_POWER,
        timeoutMs: 10000
      });
      lat = loc.latitude;
      lon = loc.longitude;
    } catch (e) {
      console.error(`[GeoService] getCurrentLocation fail: ${JSON.stringify(e)}`);
      GeoService.failReason = 'locate';
      return null;
    }
    if (!(lat !== 0 || lon !== 0)) {
      GeoService.failReason = 'locate';
      return null;
    }

    // 逆地理解析链：系统（免费）→ 高德 → 百度；失败时仅返回经纬度
    let name = await GeoService.regeoSystem(lat, lon);
    if (name.length === 0 && AMAP_WEB_KEY.length > 0) name = await GeoService.regeoAmap(lat, lon);
    if (name.length === 0 && BAIDU_AK.length > 0) name = await GeoService.regeoBaidu(lat, lon);
    const place: GeoPlace = { name: name, lat: lat, lon: lon };
    GeoService.cachedPlace = place;
    GeoService.cachedMs = now;
    return place;
  }

  // ★ 最近一次失败原因：''=成功 / 'permission' / 'disabled' / 'locate'
  static lastFailReason(): string {
    return GeoService.failReason;
  }

  // ★ 系统 Geocoder（免费、无 Key）：市 + 区 + 街道 组合
  private static async regeoSystem(lat: number, lon: number): Promise<string> {
    try {
      const list = await geoLocationManager.getAddressesFromLocation({
        latitude: lat,
        longitude: lon,
        maxItems: 1
      });
      if (list && list.length > 0) {
        const a = list[0];
        const parts: string[] = [];
        if (a.locality && a.locality.length > 0) parts.push(a.locality);
        if (a.subLocality && a.subLocality.length > 0) parts.push(a.subLocality);
        if (a.roadName && a.roadName.length > 0) parts.push(a.roadName);
        if (parts.length === 0 && a.placeName && a.placeName.length > 0) parts.push(a.placeName);
        if (parts.length === 0 && a.administrativeArea && a.administrativeArea.length > 0) {
          parts.push(a.administrativeArea);
        }
        return parts.join(' ');
      }
    } catch (e) {
      console.warn(`[GeoService] system regeo fail: ${JSON.stringify(e)}`);
    }
    return '';
  }

  private static async regeoAmap(lat: number, lon: number): Promise<string> {
    return await GeoService.httpGetText(
      `https://restapi.amap.com/v3/geocode/regeo?key=${AMAP_WEB_KEY}` +
      `&location=${lon.toFixed(6)},${lat.toFixed(6)}&extensions=base`,
      (body: string): string => {
        try {
          const resp = JSON.parse(body) as AmapRegeoResp;
          if (resp.status === '1' && resp.regeocode && resp.regeocode.formatted_address) {
            return resp.regeocode.formatted_address;
          }
        } catch (_) {}
        return '';
      });
  }

  private static async regeoBaidu(lat: number, lon: number): Promise<string> {
    return await GeoService.httpGetText(
      `https://api.map.baidu.com/reverse_geocoding/v3/?ak=${BAIDU_AK}` +
      `&output=json&coordtype=wgs84ll&location=${lat.toFixed(6)},${lon.toFixed(6)}`,
      (body: string): string => {
        try {
          const resp = JSON.parse(body) as BaiduRegeoResp;
          if (resp.status === '0' && resp.result && resp.result.formatted_address) {
            return resp.result.formatted_address;
          }
        } catch (_) {}
        return '';
      });
  }

  // ★ 通用 HTTP GET：8s 超时，200 且 body 非空才交给解析回调
  private static async httpGetText(url: string,
    parse: (body: string) => string): Promise<string> {
    const req = http.createHttp();
    try {
      const resp = await req.request(url, {
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 8000,
        readTimeout: 8000
      });
      if (resp.responseCode === 200 && typeof resp.result === 'string' && resp.result.length > 0) {
        return parse(resp.result);
      }
    } catch (e) {
      console.warn(`[GeoService] http fail: ${JSON.stringify(e)}`);
    } finally {
      req.destroy();
    }
    return '';
  }
}
