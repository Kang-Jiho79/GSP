-- monster_ai.lua
function ProcessMonsterAI(monster_id, target_player_id)
    local mon_type, mon_move_type = API_GetMonsterType(monster_id)
    local cur_x, cur_y = API_GetObjPos(monster_id)
    local spawn_x, spawn_y = API_GetSpawnPos(monster_id)
    
    local is_boss = API_IsBoss(monster_id)

    if target_player_id ~= -1 then
        local plr_x, plr_y = API_GetObjPos(target_player_id)
        
        if plr_x ~= nil then
            local dist_x = math.abs(cur_x - plr_x)
            local dist_y = math.abs(cur_y - plr_y)
            local distance = math.max(dist_x, dist_y)

            if is_boss then
                if distance <= 10 then
                    local cast_success = API_BossCastEarthquake(monster_id, target_player_id)
                    
                    if not cast_success then
                        if dist_x <= 1 and dist_y <= 1 then
                            API_MonsterAttack(monster_id, target_player_id)
                        else
                            API_MoveTowards(monster_id, target_player_id)
                        end
                    end
                    return
                end
            else
                if (mon_type == "Agro" and distance <= 5) or (mon_type == "Peace") then
                    
                    if dist_x <= 1 and dist_y <= 1 then
                        API_MonsterAttack(monster_id, target_player_id)
                    else
                        API_MoveTowards(monster_id, target_player_id)
                    end
                    return
                end
            end
        end
    end

    if mon_move_type == "고정" then
        if cur_x ~= spawn_x or cur_y ~= spawn_y then
            API_MoveToSpawn(monster_id)
        end
    elseif mon_move_type == "로밍" then
        local rand_x = spawn_x + math.random(-10, 10)
        local rand_y = spawn_y + math.random(-10, 10)
        
        API_RoamTo(monster_id, rand_x, rand_y)
    end
end