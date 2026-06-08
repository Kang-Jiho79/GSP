-- monster_ai.lua

function ProcessMonsterAI(monster_id, target_player_id)
    -- C++에서 몬스터의 고유 속성(타입, 스폰 위치, 현재 위치)을 실시간으로 가져옵니다.
    local mon_type, mon_move_type = API_GetMonsterType(monster_id)
    local cur_x, cur_y = API_GetObjPos(monster_id)
    local spawn_x, spawn_y = API_GetSpawnPos(monster_id)

    -- 1?? [전투 및 추적 로직]
    if target_player_id ~= -1 then
        local plr_x, plr_y = API_GetObjPos(target_player_id)
        
        if plr_x ~= nil then
            local dist_x = math.abs(cur_x - plr_x)
            local dist_y = math.abs(cur_y - plr_y)
            local distance = math.max(dist_x, dist_y)

            -- Agro 몬스터는 근처 11x11 영역(반경 5칸 이내)에 접근하면 쫓아옴
            -- Peace 몬스터는 때리기 전엔 안 쫓아오지만, 타겟이 잡혔다는 건 선공을 당했다는 뜻이므로 쫓아감
            if (mon_type == "Agro" and distance <= 5) or (mon_type == "Peace") then
                
                -- 공격 범위는 몬스터 기준 주변 1칸 (대각선 포함)
                if dist_x <= 1 and dist_y <= 1 then
                    API_MonsterAttack(monster_id, target_player_id)
                else
                    -- 사거리 밖이면 A* 알고리즘으로 추적
                    API_MoveTowards(monster_id, target_player_id)
                end
                return
            end
        end
    end

    -- 2?? [평화 상태 및 이동 로직] (타겟이 없거나 시야를 벗어난 경우)
    if mon_move_type == "고정" then
        -- 원래 자리(spawn_x, spawn_y)로 복귀 시도 (이미 제자리면 가만히 있음)
        if cur_x ~= spawn_x or cur_y ~= spawn_y then
            API_MoveToSpawn(monster_id)
        end
    elseif mon_move_type == "로밍" then
        -- 원래 위치에서 20x20 공간(반경 10칸 이내)을 자유롭게 이동
        local rand_x = spawn_x + math.random(-10, 10)
        local rand_y = spawn_y + math.random(-10, 10)
        
        -- C++ A* 탐색을 이용해 로밍 타겟 좌표로 안전하게 한 칸 이동
        API_RoamTo(monster_id, rand_x, rand_y)
    end
end