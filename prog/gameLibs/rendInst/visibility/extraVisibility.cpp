#include "extraVisibility.h"
#include <rendInst/visibility.h>

#include <util/dag_threadPool.h>
#include <util/dag_convar.h>

#include "riGen/riGenData.h"
#include "riGen/riGenExtra.h"
#include "riGen/rendInstTiledScene.h"
#include "render/extra/riExtraRendererT.h"
#include "visibility/cullJobRing.h"
#include "visibility/genVisibility.h"


bool rendinst::isRiGenVisibilityLodsLoaded(const RiGenVisibility *visibility)
{
  const RiGenExtraVisibility &v = visibility->riex;
  dag::ConstSpan<uint16_t> riResOrder = v.riexPoolOrder;
  for (int k = 0, ke = riResOrder.size(); k < ke; ++k)
  {
    auto ri_idx = riResOrder[k] & render::RI_RES_ORDER_COUNT_MASK;
    RenderableInstanceLodsResource *res = rendinst::riExtra[ri_idx].res;
    int bestLod = res->getQlBestLod();
    if (bestLod > v.forcedExtraLod)
    {
      res->updateReqLod(min<int>(v.forcedExtraLod, res->lods.size() - 1));
      return false;
    }
  }
  return true;
}

#if DAGOR_DBGLEVEL > 0
CONSOLE_INT_VAL("rendinst", parallel_for, 7, 0, 8);
#endif

template <typename T>
static inline T *append_data(SmallTab<T> &data, const uint32_t vecs_count)
{
  const size_t cSize = data.size();
  data.resize(cSize + vecs_count);
  return data.data() + cSize;
}

// TODO: what is this? Why is it here???
namespace rendinst::gen
{
extern bool custom_trace_ray_earth(const Point3 &src, const Point3 &dir, real &dist);
};

static void sortByPoolSizeOrder(RiGenExtraVisibility &v, int start_lod)
{
  // todo: use predefined order based on pool bbox size, so we render first biggest
  for (unsigned pool = 0; pool < v.riexData[0].size(); ++pool)
  {
    int lod = start_lod;
    for (; lod >= 0; --lod)
      if (v.riexData[lod][pool].size())
        break;
    if (lod >= 0)
      v.riexPoolOrder.push_back(pool);
  }
}

static inline void swap_data(SmallTab<vec4f> &data, uint32_t i0, uint32_t i1, const uint32_t vecs_count)
{
  vec4f temp[32];
  G_ASSERT(vecs_count < countof(temp));
  i0 *= vecs_count;
  i1 *= vecs_count;
  const uint32_t dataSize = vecs_count * sizeof(vec4f);
  memcpy(temp, data.data() + i1, dataSize);
  memcpy(data.data() + i1, data.data() + i0, dataSize);
  memcpy(data.data() + i0, temp, dataSize);
}

static constexpr int MAX_OPTIMIZATION_INSTANCES = 3;
static constexpr float MIN_OPTIMIZATION_DIST = 90.f;


static eastl::pair<int, int> scene_range_from_visiblity_rendering_flags(rendinst::VisibilityRenderingFlags flags)
{
  eastl::pair<int, int> result{0, rendinst::riExTiledScenes.size()};

  // scene array structure is as follows;
  // [dynamic scene, static scene 1, static scene 2, ... static scene n]

  if (!(flags & rendinst::VisibilityRenderingFlag::Dynamic))
    result.first = rendinst::STATIC_SCENES_START;
  if (!(flags & rendinst::VisibilityRenderingFlag::Static))
    result.second = rendinst::STATIC_SCENES_START;

  return result;
}

template <bool use_external_filter>
bool rendinst::prepareExtraVisibilityInternal(mat44f_cref globtm_cull, const Point3 &camera_pos, RiGenVisibility &vbase,
  bool render_for_shadow, Occlusion *use_occlusion, rendinst::RiExtraCullIntention cullIntention, bool for_visual_collision,
  bool filter_rendinst_clipmap, bool for_vsm, const rendinst::VisibilityExternalFilter &external_filter)
{
  if (!RendInstGenData::renderResRequired || !maxExtraRiCount || RendInstGenData::isLoading)
    return false;
  TIME_PROFILE(riextra_visibility);
  const VisibilityRenderingFlags rendering = vbase.rendering;
  RiGenExtraVisibility &v = vbase.riex;
  v.sortedTransparentElems.clear();
  if (ri_game_render_mode == 0)
    use_occlusion = nullptr;
  mat44f globtm = globtm_cull;
#if DAGOR_DBGLEVEL > 0
  if (!render_for_shadow && use_occlusion)
    globtm = use_occlusion->getCurViewProj(); // allow "frustum stop" (add_occlusion console command)
#endif
  v.vbExtraGeneration = INVALID_VB_EXTRA_GEN;

  Point3_vec4 vpos = camera_pos;
  float distSqMul = rendinst::riExtraCullDistSqMul;
  vec4f vpos_distscale = v_perm_xyzd((vec4f &)vpos, v_splats(v.forcedExtraLod >= 0 ? -1.f : distSqMul));

  const auto &poolInfo = riExTiledScenes.getPools();

  for (int lod = 0; lod < rendinst::RiExtraPool::MAX_LODS; ++lod)
  {
    clear_and_resize(v.riexData[lod], poolInfo.size());
    clear_and_resize(v.minSqDistances[lod], poolInfo.size());
    memset(v.minSqDistances[lod].data(), 0x7f, data_size(v.minSqDistances[lod])); // ~FLT_MAX
    for (auto &vv : v.riexData[lod])
      vv.resize(0);
  }

  v.riexPoolOrder.resize(0);
  v.riexInstCount = 0;
  if (!riExTiledScenes.size())
    return false;
  uint32_t additionalData = riExTiledScenes[0].getUserDataWordCount(); // in dwords
  for (const auto &tiled_scene : riExTiledScenes.scenes())
  {
    G_ASSERTF(!additionalData || !tiled_scene.getUserDataWordCount() || additionalData == tiled_scene.getUserDataWordCount(),
      " %d == %d", additionalData, tiled_scene.getUserDataWordCount());
    if (!additionalData)
      additionalData = tiled_scene.getUserDataWordCount();
  }

  // can be made invisibleFlag, if testFlags = RendinstTiledScene::VISIBLE_0, equalFlags = ~RendinstTiledScene::VISIBLE_0;
  uint32_t visibleFlag = ri_game_render_mode == 0   ? RendinstTiledScene::VISIBLE_0
                         : ri_game_render_mode == 2 ? RendinstTiledScene::VISIBLE_2
                                                    : 0;
  const auto [firstScene, lastScene] = scene_range_from_visiblity_rendering_flags(rendering);
  const int sceneCount = lastScene - firstScene;

  const bool sortLarge = !render_for_shadow && use_occlusion && check_occluders;
  static constexpr int LARGE_LOD_CNT = RiGenExtraVisibility::LARGE_LOD_CNT;

  if (sortLarge)
  {
    for (int lod = 0; lod < LARGE_LOD_CNT; ++lod)
      clear_and_resize(v.riexLarge[lod], poolInfo.size());
  }

  int newVisCnt = 0;

#define LAMBDA_BODY(forced_extra_lod_less_then_zero)                                                                            \
  G_UNUSED(ni);                                                                                                                 \
  if (render_for_shadow && scene::check_node_flags(m, RendinstTiledScene::CHECKED_IN_SHADOWS) &&                                \
      !scene::check_node_flags(m, RendinstTiledScene::VISIBLE_IN_SHADOWS | RendinstTiledScene::NEEDS_CHECK_IN_SHADOW))          \
    return;                                                                                                                     \
  if (filter_rendinst_clipmap && !scene::check_node_flags(m, RendinstTiledScene::IS_RENDINST_CLIPMAP))                          \
    return;                                                                                                                     \
  if (use_external_filter)                                                                                                      \
  {                                                                                                                             \
    vec4f bboxmin, bboxmax;                                                                                                     \
    vec4f sphere = scene::get_node_bsphere(m);                                                                                  \
    vec4f rad = v_splat_w(sphere);                                                                                              \
    bboxmin = v_sub(sphere, rad);                                                                                               \
    bboxmax = v_add(sphere, rad);                                                                                               \
    if (!external_filter(bboxmin, bboxmax))                                                                                     \
      return;                                                                                                                   \
  }                                                                                                                             \
  const scene::pool_index poolId = scene::get_node_pool(m);                                                                     \
  const auto &riPool = poolInfo[poolId];                                                                                        \
  const unsigned llm = riPool.lodLimits >> ((ri_game_render_mode + 1) * 8);                                                     \
  const unsigned min_lod = llm & 0xF, max_lod = (llm >> 4) & 0xF;                                                               \
  unsigned lod = 0;                                                                                                             \
  float dist = v_extract_x(distSqScaled);                                                                                       \
  if (forced_extra_lod_less_then_zero)                                                                                          \
  {                                                                                                                             \
    lod = find_lod<rendinst::RiExtraPool::MAX_LODS>(riPool.distSqLOD, v_extract_x(distSqScaled));                               \
    if (lod > max_lod)                                                                                                          \
      return;                                                                                                                   \
  }                                                                                                                             \
  else                                                                                                                          \
    lod = forcedExtraLod;                                                                                                       \
  lod = clamp(lod, min_lod, max_lod);                                                                                           \
  vec4f *addData = append_data(v.riexData[lod].data()[poolId], RIEXTRA_VECS_COUNT);                                             \
  v.minSqDistances[lod].data()[poolId] = min(v.minSqDistances[lod].data()[poolId], dist);                                       \
  const int32_t *userData = tiled_scene.getUserData(ni);                                                                        \
  if (userData)                                                                                                                 \
    eastl::copy_n(userData, tiled_scene.getUserDataWordCount(), (uint32_t *)(addData + rendinst::render::ADDITIONAL_DATA_IDX)); \
  v_mat44_transpose_to_mat43(*(mat43f *)addData, m);                                                                            \
  uint32_t perDataBufferOffset = poolId * (sizeof(rendinst::render::RiShaderConstBuffers) / sizeof(vec4f)) + 1;                 \
  addData[rendinst::render::ADDITIONAL_DATA_IDX] =                                                                              \
    v_perm_xaxa(addData[rendinst::render::ADDITIONAL_DATA_IDX], v_cast_vec4f(v_splatsi(perDataBufferOffset)));                  \
  newVisCnt++;


#define LAMBDA(forced_extra_lod_less_then_zero) \
  [&](scene::node_index ni, mat44f_cref m, vec4f distSqScaled) { LAMBDA_BODY(forced_extra_lod_less_then_zero) }

  SmallTab<Point2, framemem_allocator> perPoolMinDist;
  SmallTab<RiGenExtraVisibility::UVec2, framemem_allocator> perPoolBestId;

  if (!render_for_shadow && use_occlusion && check_occluders) // occlusion
  {
    G_ASSERT(v.forcedExtraLod < 0);
    const int forcedExtraLod = -1;
    int eff_num_tp_workers = threadpool::get_num_workers();
    eff_num_tp_workers = (eff_num_tp_workers > 1) ? eff_num_tp_workers : 0; // special case for 1 threadpool worker that can only serve
                                                                            // low prio jobs
    const int max_avail_threads = min(eff_num_tp_workers + (is_main_thread() ? 1 : 0), rendinst::MAX_CULL_JOBS + 1);
#if DAGOR_DBGLEVEL > 0
    if (parallel_for.get() > max_avail_threads)
      parallel_for.set(max_avail_threads);
    const int threads = parallel_for.get();
#else
    const int threads = max_avail_threads > 1 ? max_avail_threads : 0;
#endif

    perPoolMinDist.resize(poolInfo.size() * max(threads, 1));
    memset(perPoolMinDist.data(), 0x7f, data_size(perPoolMinDist)); // ~FLT_MAX
    perPoolBestId.resize(poolInfo.size() * max(threads, 1));
    memset(perPoolBestId.data(), 0xff, data_size(perPoolBestId));

    if (threads)
    {
      dag::ConstSpan<RendinstTiledScene> cscenes = riExTiledScenes.cscenes(firstScene, sceneCount);
      scene::TiledSceneCullContext *sceneContexts =
        (scene::TiledSceneCullContext *)alloca(sizeof(scene::TiledSceneCullContext) * cscenes.size());
      std::atomic<uint32_t> *riexDataCnt =
        (std::atomic<uint32_t> *)alloca(poolInfo.size() * rendinst::RiExtraPool::MAX_LODS * sizeof(std::atomic<uint32_t>));
      std::atomic<uint32_t> *riexLargeCnt =
        sortLarge ? (std::atomic<uint32_t> *)alloca(poolInfo.size() * LARGE_LOD_CNT * sizeof(std::atomic<uint32_t>)) : nullptr;
      for (int tiled_scene_idx = 0; tiled_scene_idx < cscenes.size(); tiled_scene_idx++)
        new (sceneContexts + tiled_scene_idx, _NEW_INPLACE) scene::TiledSceneCullContext;

      memset(riexDataCnt, 0, poolInfo.size() * rendinst::RiExtraPool::MAX_LODS * sizeof(riexDataCnt[0]));
      if (riexLargeCnt)
        memset(riexLargeCnt, 0, poolInfo.size() * LARGE_LOD_CNT * sizeof(riexDataCnt[0]));

      CullJobSharedState cull_sd;
      cull_sd.globtm = globtm;
      cull_sd.vpos_distscale = vpos_distscale;
      cull_sd.use_occlusion = use_occlusion;
      cull_sd.v = &v;
      cull_sd.scenes.set(cscenes.data(), cscenes.size());
      cull_sd.sceneContexts = sceneContexts;
      cull_sd.poolInfo = &poolInfo;
      cull_sd.riexDataCnt = riexDataCnt;
      cull_sd.riexLargeCnt = riexLargeCnt;
      cull_sd.perPoolMinDist = &perPoolMinDist;
      cull_sd.perPoolBestId = &perPoolBestId;

      // we should lock for reading before processing
      for (int tiled_scene_idx = 0; tiled_scene_idx < cscenes.size(); tiled_scene_idx++)
        sceneContexts[tiled_scene_idx].needToUnlock = cscenes[tiled_scene_idx].lockForRead();

      CullJobRing ring;
      ring.start(threads, cull_sd);

      for (int lod = 0; lod < rendinst::RiExtraPool::MAX_LODS; ++lod)
        for (auto &vv : v.riexData[lod])
          vv.resize(vv.capacity());
      if (sortLarge)
        for (int lod = 0; lod < LARGE_LOD_CNT; ++lod)
          for (auto &vv : v.riexLarge[lod])
            vv.resize(vv.capacity());
      for (int tiled_scene_idx = 0; tiled_scene_idx < cscenes.size(); tiled_scene_idx++)
      {
        const auto &tiled_scene = cscenes[tiled_scene_idx];
        if (visibleFlag)
          tiled_scene.frustumCullTilesPass<true, true, true>(globtm, vpos_distscale, visibleFlag, visibleFlag, use_occlusion,
            sceneContexts[tiled_scene_idx]);
        else
          tiled_scene.frustumCullTilesPass<false, true, true>(globtm, vpos_distscale, 0, 0, use_occlusion,
            sceneContexts[tiled_scene_idx]);
      }

      for (int tries = 2; tries > 0; tries--)
      {
        ring.finishWork();

        bool had_overflow = false;
        newVisCnt = 0;
        for (int lod = 0; lod < rendinst::RiExtraPool::MAX_LODS; ++lod)
          for (int poolId = 0; poolId < poolInfo.size(); ++poolId)
          {
            int sz = riexDataCnt[poolId * rendinst::RiExtraPool::MAX_LODS + lod];
            newVisCnt += sz;
            auto &vData = v.riexData[lod].data()[poolId];
            if (sz <= vData.size())
              vData.resize(sz);
            else
            {
              sz = (sz + 127) & ~127;
              vData.resize(0);
              vData.set_capacity(
                min(max(sz, (int)vData.capacity() * 2), (int)RIEXTRA_VECS_COUNT * riExtra[poolId].getEntitiesCount()));
              vData.resize(vData.capacity());
              had_overflow = true;
            }
            if (!riexLargeCnt || lod >= LARGE_LOD_CNT)
              continue;

            sz = riexLargeCnt[poolId * LARGE_LOD_CNT + lod];
            auto &vLarge = v.riexLarge[lod].data()[poolId];
            if (sz <= vLarge.size())
              vLarge.resize(sz);
            else
            {
              sz = (sz + 127) & ~127;
              vLarge.resize(0);
              vLarge.set_capacity(
                min(max(sz, (int)vLarge.capacity() * 2), (int)RIEXTRA_VECS_COUNT * riExtra[poolId].getEntitiesCount()));
              vLarge.resize(vLarge.capacity());
              had_overflow = true;
            }
          }

        if (!had_overflow)
          break;
        G_ASSERT(tries > 1);

        // compute once more
        memset(perPoolMinDist.data(), 0x7f, data_size(perPoolMinDist)); // ~FLT_MAX
        memset(perPoolBestId.data(), 0xff, data_size(perPoolBestId));
        memset(riexDataCnt, 0, poolInfo.size() * rendinst::RiExtraPool::MAX_LODS * sizeof(int)); //-V780
        if (riexLargeCnt)
          memset(riexLargeCnt, 0, poolInfo.size() * LARGE_LOD_CNT * sizeof(int)); //-V780

        for (int tiled_scene_idx = 0; tiled_scene_idx < cscenes.size(); tiled_scene_idx++)
          sceneContexts[tiled_scene_idx].nextIdxToProcess = 0;
        ring.start(threads, cull_sd);
      }

      for (int tiled_scene_idx = 0; tiled_scene_idx < cscenes.size(); tiled_scene_idx++)
      {
        if (sceneContexts[tiled_scene_idx].needToUnlock)
        {
          cscenes[tiled_scene_idx].unlockAfterRead();
          sceneContexts[tiled_scene_idx].needToUnlock = 0;
        }
        sceneContexts[tiled_scene_idx].~TiledSceneCullContext();
      }
      newVisCnt /= RIEXTRA_VECS_COUNT;

      // choose best of the bests
      if (threads > 1) //-V547
        for (int poolId = 0; poolId < poolInfo.size(); poolId++)
        {
          auto *__restrict minDist = perPoolMinDist.data() + poolId;
          auto *__restrict bestId = perPoolBestId.data() + poolId;
          for (int i = 1; i < threads; i++)
          {
            float sdist = (minDist + poolInfo.size() * i)->x;
            if (sdist < minDist->x)
            {
              minDist->y = minDist->x;
              bestId->y = bestId->x;
              minDist->x = sdist;
              bestId->x = (bestId + poolInfo.size() * i)->x;
            }
            else if (sdist < minDist->y)
            {
              minDist->y = sdist;
              bestId->y = (bestId + poolInfo.size() * i)->x;
            }
            else
              continue;

            sdist = (minDist + poolInfo.size() * i)->y;
            if (sdist < minDist->x)
            {
              minDist->y = minDist->x;
              bestId->y = bestId->x;
              minDist->x = sdist;
              bestId->x = (bestId + poolInfo.size() * i)->y;
            }
            else if (sdist < minDist->y)
            {
              minDist->y = sdist;
              bestId->y = (bestId + poolInfo.size() * i)->y;
            }
          }
        }
    }
    else
      for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
      {
        tiled_scene.template frustumCull<false, true, true>(globtm, vpos_distscale, 0, 0, use_occlusion,
          [&](scene::node_index ni, mat44f_cref m, vec4f distSqScaled) {
            LAMBDA_BODY(forcedExtraLod < 0)
            const uint32_t id = v.riexData[lod].data()[poolId].size() / RIEXTRA_VECS_COUNT - 1;
            vec4f rad = scene::get_node_bsphere_vrad(m);
            rad = v_div_x(distSqScaled, v_mul_x(rad, rad));
            float sdist = v_extract_x(rad);
            if (sortLarge && lod < LARGE_LOD_CNT && (scene::check_node_flags(m, RendinstTiledScene::LARGE_OCCLUDER)))
            {
              // this is almost as fast as using dist2 and is technically more correct.
              // however, since large occluders are usually not scaled, their radius is constant, and
              //  v_dot3_x(sphere, sort_dir) isn't that much different from projected dist
              // vec4f sphere = scene::get_node_bsphere(m);
              // float sdist = v_extract_x(v_sub(v_dot3_x(sphere, sort_dir), v_splat_w(sphere)));
              v.riexLarge[lod].data()[poolId].push_back({bitwise_cast<int, float>(sdist), id});
            }

            if (sdist < perPoolMinDist.data()[poolId].x)
            {
              perPoolMinDist.data()[poolId].y = perPoolMinDist.data()[poolId].x;
              perPoolBestId.data()[poolId].y = perPoolBestId.data()[poolId].x;
              perPoolMinDist.data()[poolId].x = sdist;
              perPoolBestId.data()[poolId].x = (id) | (lod << 28);
            }
            else if (sdist < perPoolMinDist.data()[poolId].y)
            {
              perPoolMinDist.data()[poolId].y = sdist;
              perPoolBestId.data()[poolId].y = (id) | (lod << 28);
            }
          });
        // store
      }
  }
  else if (render_for_shadow && use_occlusion && check_occluders) // shadow occlusion
  {
    G_ASSERT(v.forcedExtraLod < 0); // can't be forced lod in main csm
    const int forcedExtraLod = -1;
    const uint32_t useFlags = RendinstTiledScene::HAVE_SHADOWS | visibleFlag;
    for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
    {
      // we intentionally do not use use_flags template arg here, as virtually all nodes have shadows
      tiled_scene.template frustumCull<false, true, false>(globtm, vpos_distscale, useFlags, useFlags, nullptr,
        [&](scene::node_index ni, mat44f_cref m, vec4f distSqScaled) {
          uint8_t instLightDist = tiled_scene.getDistanceMT(ni);
          if (instLightDist < tiled_scene.LIGHTDIST_TOOBIG)
          {
            vec4f sphere = scene::get_node_bsphere(m);
            if (instLightDist < tiled_scene.LIGHTDIST_DYNAMIC)
            {
              vec4f rad = v_splat_w(sphere);
              vec3f top_point = v_add(sphere, v_and(v_cast_vec4f(V_CI_MASK0100), rad));
              if (instLightDist == tiled_scene.LIGHTDIST_INVALID)
              {
                instLightDist = tiled_scene.LIGHTDIST_TOOBIG;
                Point3_vec4 topPos;
                v_st(&topPos.x, top_point);
                float dist = 128.f;
                if (rendinst::gen::custom_trace_ray_earth(topPos, rendinst::render::dir_from_sun, dist)) // fixme: currently
                                                                                                         // dir_from_sun is not set
                                                                                                         // until first update
                                                                                                         // impostors
                  instLightDist = int(ceilf(dist)) + 1;
                tiled_scene.setDistanceMT(ni, instLightDist);
              }

              vec3f lightDist =
                v_mul(v_cvt_vec4f(v_splatsi(instLightDist)), reinterpret_cast<vec4f &>(rendinst::render::dir_from_sun));
              vec3f far_point = v_add(top_point, lightDist);
              bbox3f worldBox;
              worldBox.bmin = v_min(far_point, v_sub(sphere, rad));
              worldBox.bmax = v_max(far_point, v_add(sphere, rad));
              if (!use_occlusion->isVisibleBox(worldBox.bmin, worldBox.bmax)) // may be we should also use isOccludedBox here?
                return;
            }
            else // dynamic object
            {
              // replace with bounding sphere
              if (use_occlusion->isOccludedSphere(sphere, v_splat_w(v_add(sphere, sphere)))) //
                return;
            }
          }
          if (!scene::check_node_flags(m, RendinstTiledScene::HAVE_SHADOWS)) // we still have to check flag, but we assume it will
                                                                             // happen very rare that it fails, so check it last
            return;
          LAMBDA_BODY(forcedExtraLod < 0);
        });
    }
  }
  else if (cullIntention != RiExtraCullIntention::MAIN)
  {
    uint32_t useFlags = visibleFlag;
    bool depthOrReflectino = cullIntention == RiExtraCullIntention::DRAFT_DEPTH || cullIntention == RiExtraCullIntention::REFLECTIONS;
    G_ASSERT(v.forcedExtraLod < 0 || !depthOrReflectino);
    const int forcedExtraLod = depthOrReflectino ? -1 : v.forcedExtraLod; // can't be forced lod for depth/reflections
    if (cullIntention == RiExtraCullIntention::DRAFT_DEPTH)
    {
      useFlags |= RendinstTiledScene::DRAFT_DEPTH;
    }
    else if (cullIntention == RiExtraCullIntention::REFLECTIONS)
    {
      useFlags |= RendinstTiledScene::REFLECTION;
    }
    else if (cullIntention == RiExtraCullIntention::LANDMASK)
    {
      useFlags |= RendinstTiledScene::VISIBLE_IN_LANDMASK;
    }
    for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
    {
      tiled_scene.template frustumCull<true, true, false>(globtm, vpos_distscale, useFlags, useFlags, nullptr,
        LAMBDA(forcedExtraLod < 0));
    }
  }
  else if (for_visual_collision) // phydetails
  {
    const int forcedExtraLod = v.forcedExtraLod;
    const uint32_t useFlags = RendinstTiledScene::VISUAL_COLLISION | visibleFlag;
    for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
    {
      tiled_scene.template frustumCull<true, true, false>(globtm, vpos_distscale, useFlags, useFlags, nullptr,
        LAMBDA(forcedExtraLod < 0));
    }
  }
  else if (for_vsm) // phydetails
  {
    const int forcedExtraLod = v.forcedExtraLod;
    const uint32_t useFlags = RendinstTiledScene::VISIBLE_IN_VSM | visibleFlag;
    for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
    {
      tiled_scene.template frustumCull<true, true, false>(globtm, vpos_distscale, useFlags, useFlags, nullptr,
        LAMBDA(forcedExtraLod < 0));
    }
  }
  else
  {
    const int forcedExtraLod = v.forcedExtraLod;
    const uint32_t useFlags = visibleFlag;
    //
    if (useFlags == 0)
    {
      if (forcedExtraLod >= 0) // we just hope that compiler will optimize code inside lambda with it. Although it is possible that it
                               // won't, than can copy-paste lambda code
        for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
          tiled_scene.template frustumCull<false, true, false>(globtm, vpos_distscale, 0, 0, nullptr, LAMBDA(false));
      else
        for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
          tiled_scene.template frustumCull<false, true, false>(globtm, vpos_distscale, 0, 0, nullptr, LAMBDA(true));
    }
    else
    {
      if (forcedExtraLod >= 0) // we just hope that compiler will optimize code inside lambda with it. Although it is possible that it
                               // won't, than can copy-paste lambda code
        for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
          tiled_scene.template frustumCull<true, true, false>(globtm, vpos_distscale, useFlags, useFlags, nullptr, LAMBDA(false));
      else
        for (const auto &tiled_scene : riExTiledScenes.cscenes(firstScene, sceneCount))
          tiled_scene.template frustumCull<true, true, false>(globtm, vpos_distscale, useFlags, useFlags, nullptr, LAMBDA(true));
    }
  }
  v.riexInstCount = newVisCnt;
  // todo: if not rendering to main, use predefined pool order based on pool bbox size (for shadows and such), from big to small
  // todo: replace reflection hardcodes & guesses (minimum_size>0, and setting flag 1 if pool size > 25) with explicit logic
  // todo: auto detect params for rendinst scenes count and params based on profile guided distances
  if (!v.riexInstCount)
    return true;

  {
    TIME_PROFILE(sortPool);
    if (sortLarge)
    {
      uint16_t minPool, maxPool;
      for (minPool = 0; minPool < poolInfo.size(); ++minPool)
        if (perPoolMinDist[minPool].x < bitwise_cast<float, int>(0x7f7f7f7f))
          break;
      for (maxPool = poolInfo.size() - 1; maxPool > minPool; --maxPool)
        if (perPoolMinDist[maxPool].x < bitwise_cast<float, int>(0x7f7f7f7f))
          break;

      v.riexPoolOrder.reserve(maxPool - minPool + 1);
      // cost of sort is about 0.02msec. however it speeds up rendering sometimes by 10% of GPU time
      // it can be used in shadows as well, but based on sun dir distance
      {
        SmallTab<RiGenExtraVisibility::Order, framemem_allocator> distAndPool;
        distAndPool.reserve(maxPool - minPool + 1);
        for (unsigned i = minPool; i <= maxPool; ++i)
          if (perPoolMinDist[i].x < bitwise_cast<float, int>(0x7f7f7f7f))
            distAndPool.push_back({bitwise_cast<int, float>(perPoolMinDist[i].x), i});
        stlsort::sort_branchless(distAndPool.begin(), distAndPool.end());
        v.riexPoolOrder.resize(distAndPool.size());
        for (int i = 0; i < distAndPool.size(); ++i)
          v.riexPoolOrder[i] = distAndPool[i].id;
      }
    }
    else
    {
      sortByPoolSizeOrder(v, rendinst::RiExtraPool::MAX_LODS - 1);
    }
  }

  if (sortLarge)
  {
    TIME_PROFILE(sortLarge);
    const float max_dist_to_sort = 500.f * 500.f * distSqMul;
    static int min_optimization_dist2i = bitwise_cast<int, float>(MIN_OPTIMIZATION_DIST * MIN_OPTIMIZATION_DIST * distSqMul);
    for (int lod = 0; lod < LARGE_LOD_CNT; ++lod)
      for (auto &poolAndCnt : v.riexPoolOrder)
      {
        auto poolId = poolAndCnt & render::RI_RES_ORDER_COUNT_MASK;
        auto &data = v.riexData[lod][poolId];
        if (!data.size())
          continue;
        auto &ind = v.riexLarge[lod][poolId];
        if (ind.size() && (perPoolMinDist[poolId].x < max_dist_to_sort || ind.size() < 8))
        {
          stlsort::sort_branchless(ind.begin(), ind.end());
          int id = 0;
          clear_and_resize(v.largeTempData, ind.size() * RIEXTRA_VECS_COUNT);
          for (auto i : ind)
          {
            memcpy(v.largeTempData.data() + (id++) * RIEXTRA_VECS_COUNT, data.data() + i.id * RIEXTRA_VECS_COUNT,
              RIEXTRA_VECS_COUNT * sizeof(vec4f));
          }
          memcpy(data.data(), v.largeTempData.data(), ind.size() * RIEXTRA_VECS_COUNT * sizeof(vec4f));
          if (lod == 0)
          {
            uint32_t instances = 0;
            for (int j = 0, je = min<int>(ind.size(), MAX_OPTIMIZATION_INSTANCES); j < je; ++j)
              if (ind[j].d < min_optimization_dist2i)
                instances++;
            G_ASSERT(instances <= MAX_OPTIMIZATION_INSTANCES);
            poolAndCnt |= instances << render::RI_RES_ORDER_COUNT_SHIFT;
          }
          G_STATIC_ASSERT(MAX_OPTIMIZATION_INSTANCES <= (1 << 2) - 1); // cause we just allocated 2 bits
        }
        else if (data.size() > RIEXTRA_VECS_COUNT)
        {
          auto best = perPoolBestId[poolId];
          if ((best.x >> 28) == lod)
          {
            swap_data(data, best.x & ((1 << 28) - 1), 0, RIEXTRA_VECS_COUNT);
            // eastl::swap(matrices[best.x&((1<<28)-1)], matrices[0]);
            if ((best.y >> 28) == lod)
              swap_data(data, 0 == (best.y & ((1 << 28) - 1)) ? (best.x & ((1 << 28) - 1)) : (best.y & ((1 << 28) - 1)), 1,
                RIEXTRA_VECS_COUNT);
            // eastl::swap(matrices[ 0 == (best.y&((1<<28)-1)) ? (best.x&((1<<28)-1)) : (best.y&((1<<28)-1))], matrices[1]);
          }
          else if ((best.y >> 28) == lod)
            swap_data(data, best.y & ((1 << 28) - 1), 0, RIEXTRA_VECS_COUNT);
        }
        ind.resize(0);
      }
    // sort matrices
  }

  return true;
}

void rendinst::sortTransparentRiExtraInstancesByDistance(RiGenVisibility *vb, const Point3 &view_pos)
{
  TIME_D3D_PROFILE(sort_ri_extra_per_instance);

  vec3f viewPos = v_ldu_p3_safe(&view_pos.x);

  RiGenExtraVisibility &v = vb->riex;
  auto &elems = v.sortedTransparentElems;
  elems.clear();

  dag::ConstSpan<uint16_t> riResOrder = riExPoolIdxPerStage[get_layer_index(rendinst::LayerFlag::Transparent)];
  for (int lod = 0; lod < RiExtraPool::MAX_LODS; lod++)
  {
    if (!(v.riExLodNotEmpty & (1 << lod)))
      continue;

    SmallTab<SmallTab<vec4f>> &riExDataLod = v.riexData[lod];

    for (int order = 0; order < riResOrder.size(); order++)
    {
      int poolId = riResOrder[order];
      const vec4f *data = riExDataLod[poolId].data();
      uint32_t poolCnt = (uint32_t)riExDataLod[poolId].size() / RIEXTRA_VECS_COUNT;
      for (int i = 0; i < poolCnt; i++)
      {
        mat44f tm;
        v_mat43_transpose_to_mat44(tm, *(mat43f *)data);

        vec4f instancePos = v_mat44_mul_vec3p(tm, riExtra[poolId].bsphXYZR);
        vec3f dpos = v_sub(instancePos, viewPos);
        float dist2 = v_extract_x(v_dot3_x(dpos, dpos));

        RiGenExtraVisibility::PerInstanceElem elem = {};
        elem.lod = lod;
        elem.poolId = poolId;
        elem.poolOrder = order;
        elem.dist2 = dist2;
        elem.instanceId = i;

        elems.push_back(elem);

        data += RIEXTRA_VECS_COUNT;
      }
    }
  }

  eastl::sort(elems.begin(), elems.end(),
    [](const RiGenExtraVisibility::PerInstanceElem &elem1, const RiGenExtraVisibility::PerInstanceElem &elem2) -> bool {
      return elem1.dist2 > elem2.dist2;
    });
}

bool rendinst::prepareRIGenExtraVisibility(mat44f_cref globtm_cull, const Point3 &camera_pos, RiGenVisibility &vbase,
  bool render_for_shadow, Occlusion *use_occlusion, RiExtraCullIntention cullIntention, bool for_visual_collision,
  bool filter_rendinst_clipmap, bool for_vsm, const rendinst::VisibilityExternalFilter &external_filter)
{
  if (!external_filter)
    return prepareExtraVisibilityInternal(globtm_cull, camera_pos, vbase, render_for_shadow, use_occlusion, cullIntention,
      for_visual_collision, filter_rendinst_clipmap, for_vsm);
  else
    return prepareExtraVisibilityInternal<true>(globtm_cull, camera_pos, vbase, render_for_shadow, use_occlusion, cullIntention,
      for_visual_collision, filter_rendinst_clipmap, for_vsm, external_filter);
}

bool rendinst::prepareRIGenExtraVisibilityBox(bbox3f_cref box_cull, int forced_lod, float min_size, float min_dist,
  RiGenVisibility &vbase, bbox3f *result_box)
{
  if (!RendInstGenData::renderResRequired || !maxExtraRiCount || RendInstGenData::isLoading)
    return false;
  TIME_PROFILE(riextra_visibility_box);
  const VisibilityRenderingFlags rendering = vbase.rendering;
  RiGenExtraVisibility &v = vbase.riex;
  v.vbExtraGeneration = INVALID_VB_EXTRA_GEN;

  const auto &poolInfo = riExTiledScenes.getPools();

  for (int lod = 0; lod < rendinst::RiExtraPool::MAX_LODS; ++lod)
  {
    clear_and_resize(v.riexData[lod], poolInfo.size());
    clear_and_resize(v.minSqDistances[lod], poolInfo.size());
    memset(v.minSqDistances[lod].data(), 0x7f, data_size(v.minSqDistances[lod])); // ~FLT_MAX
    for (auto &vv : v.riexData[lod])
      vv.resize(0);
  }
  forced_lod = clamp(forced_lod, 0, rendinst::RiExtraPool::MAX_LODS - 1);

  v.riexPoolOrder.resize(0);
  if (!riExTiledScenes.size())
  {
    v.riexInstCount = 0;
    return false;
  }
  uint32_t additionalData = riExTiledScenes[0].getUserDataWordCount(); // in dwords
  for (const auto &tiled_scene : riExTiledScenes.scenes())
  {
    G_ASSERTF(!additionalData || !tiled_scene.getUserDataWordCount() || additionalData == tiled_scene.getUserDataWordCount(),
      " %d == %d", additionalData, tiled_scene.getUserDataWordCount());
    if (!additionalData)
      additionalData = tiled_scene.getUserDataWordCount();
  }

  // can be made invisibleFlag, if testFlags = RendinstTiledScene::VISIBLE_0, equalFlags = ~RendinstTiledScene::VISIBLE_0;
  // uint32_t visibleFlag = ri_game_render_mode == 0 ? RendinstTiledScene::VISIBLE_0 : 0;//todo:? support flags?
  const auto [firstScene, lastScene] = scene_range_from_visiblity_rendering_flags(rendering);

  int newVisCnt = 0;
  vec4f min_size_v = v_splats(min_size);
  int maxLodUsed = forced_lod;
  if (result_box)
    v_bbox3_init_empty(*result_box);

  for (int scnI = firstScene; scnI < lastScene; ++scnI)
  {
    if (riExTiledSceneMaxDist[scnI] <= min_dist) // skip it anyway, all it's data will be of smaller size
      continue;
    float min_dist_sq = min_dist * min_dist;
    const auto &tiled_scene = riExTiledScenes[scnI];
    tiled_scene.boxCull<false, true>(box_cull, 0, 0, [&](scene::node_index ni, mat44f_cref m) {
      if (v_test_vec_x_lt(scene::get_node_bsphere_vrad(m), min_size_v))
        return;
      const scene::pool_index poolId = scene::get_node_pool(m);
      const auto &riPool = poolInfo[poolId];
      if (riPool.distSqLOD[rendinst::RiExtraPool::MAX_LODS - 1] < min_dist_sq)
        return;
      const unsigned llm = riPool.lodLimits >> ((ri_game_render_mode + 1) * 8);
      const unsigned min_lod = llm & 0xF, max_lod = (llm >> 4) & 0xF;
      int lod = clamp((unsigned)forced_lod, min_lod, max_lod);
      maxLodUsed = max(lod, maxLodUsed);
      vec4f *addData = append_data(v.riexData[lod].data()[poolId], RIEXTRA_VECS_COUNT);
      const int32_t *userData = tiled_scene.getUserData(ni);
      if (userData)
        eastl::copy_n(userData, tiled_scene.getUserDataWordCount(), (uint32_t *)(addData + rendinst::render::ADDITIONAL_DATA_IDX));
      v_mat44_transpose_to_mat43(*(mat43f *)addData, m);
      if (result_box)
        v_bbox3_add_box(*result_box, tiled_scene.calcNodeBox(m));
      uint32_t perDataBufferOffset = poolId * (sizeof(rendinst::render::RiShaderConstBuffers) / sizeof(vec4f)) + 1;
      addData[rendinst::render::ADDITIONAL_DATA_IDX] =
        v_perm_xaxa(addData[rendinst::render::ADDITIONAL_DATA_IDX], v_cast_vec4f(v_splatsi(perDataBufferOffset)));
      newVisCnt++;
    });
  }
  v.riexInstCount = newVisCnt;
  // todo: if not rendering to main, use predefined pool order based on pool bbox size (for shadows and such), from big to small
  // todo: replace reflection hardcodes & guesses (minimum_size>0, and setting flag 1 if pool size > 25) with explicit logic
  // todo: auto detect params for rendinst scenes count and params based on profile guided distances
  if (!v.riexInstCount)
    return true;

  sortByPoolSizeOrder(v, maxLodUsed);

  return true;
}
