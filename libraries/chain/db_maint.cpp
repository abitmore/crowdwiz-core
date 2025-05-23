/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <boost/multiprecision/integer.hpp>

#include <fc/uint128.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/fba_accumulator_id.hpp>
#include <graphene/chain/hardfork.hpp>

#include <graphene/chain/account_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/budget_record_object.hpp>
#include <graphene/chain/buyback_object.hpp>
#include <graphene/chain/chain_property_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/chain/fba_object.hpp>
#include <graphene/chain/global_property_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/special_authority_object.hpp>
#include <graphene/chain/vesting_balance_object.hpp>
#include <graphene/chain/vote_count.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/chain/worker_object.hpp>

namespace graphene { namespace chain {

template<class Index>
vector<std::reference_wrapper<const typename Index::object_type>> database::sort_votable_objects(size_t count) const
{
   using ObjectType = typename Index::object_type;
   const auto& all_objects = get_index_type<Index>().indices();
   count = std::min(count, all_objects.size());
   vector<std::reference_wrapper<const ObjectType>> refs;
   refs.reserve(all_objects.size());
   std::transform(all_objects.begin(), all_objects.end(),
                  std::back_inserter(refs),
                  [](const ObjectType& o) { return std::cref(o); });
   std::partial_sort(refs.begin(), refs.begin() + count, refs.end(),
                   [this](const ObjectType& a, const ObjectType& b)->bool {
      share_type oa_vote = _vote_tally_buffer[a.vote_id];
      share_type ob_vote = _vote_tally_buffer[b.vote_id];
      if( oa_vote != ob_vote )
         return oa_vote > ob_vote;
      return a.vote_id < b.vote_id;
   });

   refs.resize(count, refs.front());
   return refs;
}

template<class Type>
void database::perform_account_maintenance(Type tally_helper)
{
   const auto& bal_idx = get_index_type< account_balance_index >().indices().get< by_maintenance_flag >();
   if( bal_idx.begin() != bal_idx.end() )
   {
      auto bal_itr = bal_idx.rbegin();
      while( bal_itr->maintenance_flag )
      {
         const account_balance_object& bal_obj = *bal_itr;

         modify( get_account_stats_by_owner( bal_obj.owner ), [&bal_obj](account_statistics_object& aso) {
            aso.core_in_balance = bal_obj.balance;
         });

         modify( bal_obj, []( account_balance_object& abo ) {
            abo.maintenance_flag = false;
         });

         bal_itr = bal_idx.rbegin();
      }
   }

   const auto& stats_idx = get_index_type< account_stats_index >().indices().get< by_maintenance_seq >();
   auto stats_itr = stats_idx.lower_bound( true );

   while( stats_itr != stats_idx.end() )
   {
      const account_statistics_object& acc_stat = *stats_itr;
      const account_object& acc_obj = acc_stat.owner( *this );
      ++stats_itr;

      if( acc_stat.has_some_core_voting() )
         tally_helper( acc_obj, acc_stat );

      if( acc_stat.has_pending_fees() )
         acc_stat.process_fees( acc_obj, *this );
   }

}

void database::perform_credit_maintenance()
{
   const auto& stats_idx = get_index_type< account_stats_index >().indices().get< by_network_income >();
   auto stats_itr = stats_idx.lower_bound( true );

   while( stats_itr != stats_idx.end() )
   {
      const account_statistics_object& acc_stat = *stats_itr;
      ++stats_itr;

      modify( acc_stat, []( account_statistics_object& aso )
      {
         aso.first_month_income=aso.second_month_income;
         aso.second_month_income=aso.third_month_income;
         aso.third_month_income=aso.current_month_income;
         aso.current_month_income=0;
      } );
   }
}

void database::perform_gr_maintenance()
{
   const auto& stats_idx = get_index_type< account_stats_index >().indices().get< by_gr_volume >();
   auto stats_itr = stats_idx.lower_bound( true );

   while( stats_itr != stats_idx.end() )
   {
      const account_statistics_object& acc_stat = *stats_itr;
      ++stats_itr;

      modify( acc_stat, []( account_statistics_object& aso )
      {
         aso.last_period_gr=aso.current_period_gr;
         aso.current_period_gr=0;
      } );
   }
}

void database::perform_p2p_maintenance()
{
   const auto& stats_idx = get_index_type< account_stats_index >().indices().get< by_p2p_rating >();
   auto stats_itr = stats_idx.lower_bound( true );

   while( stats_itr != stats_idx.end() )
   {
      const account_statistics_object& acc_stat = *stats_itr;
      ++stats_itr;

      modify( acc_stat, []( account_statistics_object& aso )
      {
         aso.p2p_first_month_rating=aso.p2p_current_month_rating;
         aso.p2p_current_month_rating=0;
      } );
   }
}

void database::count_gr_votes() {
   
   const auto& gr_votes_idx = get_index_type< gr_votes_index >().indices().get< by_id >();
   auto votes_itr = gr_votes_idx.begin();
   auto total_votes = gr_votes_idx.size();
   if (total_votes>0) {
      share_type gr_iron_volume = 0;
      share_type gr_bronze_volume = 0;
      share_type gr_silver_volume = 0;
      share_type gr_gold_volume = 0;
      share_type gr_platinum_volume = 0;
      share_type gr_diamond_volume = 0;
      share_type gr_master_volume = 0;
      share_type gr_iron_reward = 0;
      share_type gr_bronze_reward = 0;
      share_type gr_silver_reward = 0;
      share_type gr_gold_reward = 0;
      share_type gr_platinum_reward = 0;
      share_type gr_diamond_reward = 0;
      share_type gr_elite_reward = 0;
      share_type gr_master_reward = 0;

      while( votes_itr != gr_votes_idx.end() )
      {
         const gr_votes_object& vote = *votes_itr;
         gr_iron_volume+=vote.gr_iron_volume;
         gr_bronze_volume+=vote.gr_bronze_volume;
         gr_silver_volume+=vote.gr_silver_volume;
         gr_gold_volume+=vote.gr_gold_volume;
         gr_platinum_volume+=vote.gr_platinum_volume;
         gr_diamond_volume+=vote.gr_diamond_volume;
         gr_master_volume+=vote.gr_master_volume;
         gr_iron_reward+=vote.gr_iron_reward;
         gr_bronze_reward+=vote.gr_bronze_reward;
         gr_silver_reward+=vote.gr_silver_reward;
         gr_gold_reward+=vote.gr_gold_reward;
         gr_platinum_reward+=vote.gr_platinum_reward;
         gr_diamond_reward+=vote.gr_diamond_reward;
         gr_elite_reward+=vote.gr_elite_reward;
         gr_master_reward+=vote.gr_master_reward;
         ++votes_itr;
      }

      while( !gr_votes_idx.empty() )
      {
         const gr_votes_object& vote = *gr_votes_idx.begin();
         remove(vote);
      }
      const dynamic_global_property_object& dgpo = get_dynamic_global_properties();
      modify(dgpo, [&](dynamic_global_property_object& d) {
         d.gr_iron_volume=gr_iron_volume/total_votes;
         d.gr_bronze_volume=gr_bronze_volume/total_votes;
         d.gr_silver_volume=gr_silver_volume/total_votes;
         d.gr_gold_volume=gr_gold_volume/total_votes;
         d.gr_platinum_volume=gr_platinum_volume/total_votes;
         d.gr_diamond_volume=gr_diamond_volume/total_votes;
         d.gr_master_volume=gr_master_volume/total_votes;
         d.gr_iron_reward=gr_iron_reward/total_votes;
         d.gr_bronze_reward=gr_bronze_reward/total_votes;
         d.gr_silver_reward=gr_silver_reward/total_votes;
         d.gr_gold_reward=gr_gold_reward/total_votes;
         d.gr_platinum_reward=gr_platinum_reward/total_votes;
         d.gr_diamond_reward=gr_diamond_reward/total_votes;
         d.gr_elite_reward=gr_elite_reward/total_votes;
         d.gr_master_reward=gr_master_reward/total_votes;
      });
   }
}

void database::proceed_gr_top3() {
   const dynamic_global_property_object& dgpo = get_dynamic_global_properties();
   if (dgpo.current_gr_interval == 2) {
      auto& top3_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_2_volume>();
      if (top3_idx.size()>=3) {
         share_type supply=0;
         uint8_t count = 0;
         auto team_itr = top3_idx.rbegin();
         while( count < 3)
         {
            const gr_team_object& team = *team_itr;
            if (team.gr_interval_2_volume>0){
               gr_pay_top_reward_operation vop;
               vop.captain = team.captain;
               vop.team = team.id;
               vop.amount = asset( dgpo.gr_top3_reward, asset_id_type(0) );
               vop.interval = dgpo.current_gr_interval;
               push_applied_operation( vop );
               adjust_balance(team.captain, asset( dgpo.gr_top3_reward, asset_id_type(0) ));
               supply+=dgpo.gr_top3_reward;
            }
            ++count;
            ++team_itr;
         }
         modify( get_core_dynamic_data(), [supply](asset_dynamic_data_object& d) {
            d.current_supply += supply;
         });
      }
   }
   if (dgpo.current_gr_interval == 4) {
      auto& top3_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_4_volume>();
      if (top3_idx.size()>=3) {
         share_type supply=0;
         uint8_t count = 0;
         auto team_itr = top3_idx.rbegin();
         while( count < 3)
         {
            const gr_team_object& team = *team_itr;
            if (team.gr_interval_4_volume>0){
               gr_pay_top_reward_operation vop;
               vop.captain = team.captain;
               vop.team = team.id;
               vop.amount = asset( dgpo.gr_top3_reward, asset_id_type(0) );
               vop.interval = dgpo.current_gr_interval;
               push_applied_operation( vop );
               adjust_balance(team.captain, asset( dgpo.gr_top3_reward, asset_id_type(0) ));
               supply+=dgpo.gr_top3_reward;
            }
            ++count;
            ++team_itr;
         }
         modify( get_core_dynamic_data(), [supply](asset_dynamic_data_object& d) {
            d.current_supply += supply;
         });
      }
   }   
   if (dgpo.current_gr_interval == 6) {
      auto& top3_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_6_volume>();
      if (top3_idx.size()>=3) {
         share_type supply=0;
         uint8_t count = 0;
         auto team_itr = top3_idx.rbegin();
         while( count < 3)
         {
            const gr_team_object& team = *team_itr;
            if (team.gr_interval_6_volume>0){
               gr_pay_top_reward_operation vop;
               vop.captain = team.captain;
               vop.team = team.id;
               vop.amount = asset( dgpo.gr_top3_reward, asset_id_type(0) );
               vop.interval = dgpo.current_gr_interval;
               push_applied_operation( vop );
               adjust_balance(team.captain, asset( dgpo.gr_top3_reward, asset_id_type(0) ));
               supply+=dgpo.gr_top3_reward;
            }
            ++count;
            ++team_itr;
         }
         modify( get_core_dynamic_data(), [supply](asset_dynamic_data_object& d) {
            d.current_supply += supply;
         });
      }
   }
   if (dgpo.current_gr_interval == 9) {
      auto& top3_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_9_volume>();
      if (top3_idx.size()>=3) {
         share_type supply=0;
         uint8_t count = 0;
         auto team_itr = top3_idx.rbegin();
         while( count < 3)
         {
            const gr_team_object& team = *team_itr;
            if (team.gr_interval_9_volume>0){
               gr_pay_top_reward_operation vop;
               vop.captain = team.captain;
               vop.team = team.id;
               vop.amount = asset( dgpo.gr_top3_reward, asset_id_type(0) );
               vop.interval = dgpo.current_gr_interval;
               push_applied_operation( vop );
               adjust_balance(team.captain, asset( dgpo.gr_top3_reward, asset_id_type(0) ));
               supply+=dgpo.gr_top3_reward;
            }
            ++count;
            ++team_itr;
         }
         modify( get_core_dynamic_data(), [supply](asset_dynamic_data_object& d) {
            d.current_supply += supply;
         });
      }
   }
   if (dgpo.current_gr_interval == 11) {
      auto& top3_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_11_volume>();
      if (top3_idx.size()>=3) {
         share_type supply=0;
         uint8_t count = 0;
         auto team_itr = top3_idx.rbegin();
         while( count < 3)
         {
            const gr_team_object& team = *team_itr;
            if (team.gr_interval_11_volume>0){
               gr_pay_top_reward_operation vop;
               vop.captain = team.captain;
               vop.team = team.id;
               vop.amount = asset( dgpo.gr_top3_reward, asset_id_type(0) );
               vop.interval = dgpo.current_gr_interval;
               push_applied_operation( vop );
               adjust_balance(team.captain, asset( dgpo.gr_top3_reward, asset_id_type(0) ));
               supply+=dgpo.gr_top3_reward;
            }
            ++count;
            ++team_itr;
         }
         modify( get_core_dynamic_data(), [supply](asset_dynamic_data_object& d) {
            d.current_supply += supply;
         });
      }
   }   
   if (dgpo.current_gr_interval == 13) {
      auto& top3_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_13_volume>();
      if (top3_idx.size()>=3) {
         share_type supply=0;
         uint8_t count = 0;
         auto team_itr = top3_idx.rbegin();
         while( count < 3)
         {
            const gr_team_object& team = *team_itr;
            if (team.gr_interval_13_volume>0){
               gr_pay_top_reward_operation vop;
               vop.captain = team.captain;
               vop.team = team.id;
               vop.amount = asset( dgpo.gr_top3_reward, asset_id_type(0) );
               vop.interval = dgpo.current_gr_interval;
               push_applied_operation( vop );
               adjust_balance(team.captain, asset( dgpo.gr_top3_reward, asset_id_type(0) ));
               supply+=dgpo.gr_top3_reward;
            }
            ++count;
            ++team_itr;
         }
         modify( get_core_dynamic_data(), [supply](asset_dynamic_data_object& d) {
            d.current_supply += supply;
         });
      }
   }
}


void database::proceed_gr_bets() {
   // COUNT PLACES

   const dynamic_global_property_object& dgpo = get_dynamic_global_properties();
   map<gr_team_id_type, uint64_t> rating;

   if (dgpo.current_gr_interval == 2) {
      auto& team_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_2_volume>();
      uint64_t place=1;
      auto team_itr = team_idx.rbegin();
      while( team_itr != team_idx.rend() )
      {
         rating[team_itr->id]=place;
         place++;
         ++team_itr;
      } 
   }
   if (dgpo.current_gr_interval == 4) {
      auto& team_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_4_volume>();
      uint64_t place=1;
      auto team_itr = team_idx.rbegin();

      while( team_itr != team_idx.rend() )
      {
         rating[team_itr->id]=place;
         place++;
         ++team_itr;
      } 
   }   
   if (dgpo.current_gr_interval == 6) {
      auto& team_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_6_volume>();
      uint64_t place=1;
      auto team_itr = team_idx.rbegin();

      while( team_itr != team_idx.rend() )
      {
         rating[team_itr->id]=place;
         place++;
         ++team_itr;
      } 
   }
   if (dgpo.current_gr_interval == 9) {
      auto& team_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_9_volume>();
      uint64_t place=1;
      auto team_itr = team_idx.rbegin();

      while( team_itr != team_idx.rend() )
      {
         rating[team_itr->id]=place;
         place++;
         ++team_itr;
      } 
   }
   if (dgpo.current_gr_interval == 11) {
      auto& team_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_11_volume>();
      uint64_t place=1;
      auto team_itr = team_idx.rbegin();

      while( team_itr != team_idx.rend() )
      {
         rating[team_itr->id]=place;
         place++;
         ++team_itr;
      } 
   }   
   if (dgpo.current_gr_interval == 13) {
      auto& team_idx = get_index_type<gr_team_index>().indices().get<by_gr_interval_13_volume>();
      uint64_t place=1;
      auto team_itr = team_idx.rbegin();

      while( team_itr != team_idx.rend() )
      {
         rating[team_itr->id]=place;
         place++;
         ++team_itr;
      } 
   }
   // GR RANGE BETS Proceed
   const auto& gr_range_bets_idx = get_index_type< gr_range_bet_index >().indices().get< by_id >();

   auto gr_range_itr = gr_range_bets_idx.begin();
   share_type accumulated_fee = 0;
   while( gr_range_itr != gr_range_bets_idx.end() )
   {
      const gr_range_bet_object& gr_range = *gr_range_itr;
      uint64_t place = rating[gr_range.team];

      if (gr_range.true_bets.size()>0 && gr_range.false_bets.size()>0) {

         if (place >= gr_range.lower_rank && place <= gr_range.upper_rank) { 
            share_type total_payed=0;
               for( auto bet : gr_range.true_bets ) {
                  fc::uint128 prize_part_calc(gr_range.total_prize.value);
                  prize_part_calc *= bet.second.value;
                  prize_part_calc /= gr_range.total_true_bets.value;
                  uint64_t prize_part = prize_part_calc.to_uint64();
                  total_payed+=prize_part;
                  if (prize_part>0) {
                     gr_range_bet_win_operation vop;

                     vop.gr_range_bet = gr_range.id;
                     vop.team = gr_range.team;
                     vop.lower_rank = gr_range.lower_rank;
                     vop.upper_rank = gr_range.upper_rank;
                     vop.result = place;
                     vop.total_bets = asset( gr_range.total_prize, asset_id_type(0) );
                     vop.total_wins = asset(gr_range.total_true_bets, asset_id_type(0) );
                     vop.bettor_part = asset(bet.second, asset_id_type(0) );
                     vop.reward = asset( prize_part, asset_id_type(0) );
                     vop.bettor = bet.first;
                     push_applied_operation( vop );
                     adjust_balance(bet.first, asset( prize_part, asset_id_type(0)));
                  }
               }
               if (gr_range.total_prize>total_payed) {
                  accumulated_fee+=(gr_range.total_prize-total_payed);
               }
               for( auto bet : gr_range.false_bets ) { 
                  gr_range_bet_loose_operation vop;
                  vop.gr_range_bet = gr_range.id;
                  vop.team = gr_range.team;
                  vop.lower_rank = gr_range.lower_rank;
                  vop.upper_rank = gr_range.upper_rank;
                  vop.result = place;
                  vop.bettor = bet.first;
                  push_applied_operation( vop );               
               }
            }
         else {
            share_type total_payed=0;
            for( auto bet : gr_range.false_bets ) {
               fc::uint128 prize_part_calc(gr_range.total_prize.value);
               prize_part_calc *= bet.second.value;
               prize_part_calc /= gr_range.total_false_bets.value;
               uint64_t prize_part = prize_part_calc.to_uint64();
               total_payed+=prize_part;
               if (prize_part>0) {
                  gr_range_bet_win_operation vop;

                  vop.gr_range_bet = gr_range.id;
                  vop.team = gr_range.team;
                  vop.lower_rank = gr_range.lower_rank;
                  vop.upper_rank = gr_range.upper_rank;
                  vop.result = place;
                  vop.total_bets = asset( gr_range.total_prize, asset_id_type(0) );
                  vop.total_wins = asset( gr_range.total_false_bets, asset_id_type(0) );
                  vop.bettor_part = asset( bet.second, asset_id_type(0) );
                  vop.reward = asset( prize_part, asset_id_type(0) );
                  vop.bettor = bet.first;
                  push_applied_operation( vop );
                  adjust_balance(bet.first, asset( prize_part, asset_id_type(0)));
               }
            }
            if (gr_range.total_prize>total_payed) {
               accumulated_fee+=(gr_range.total_prize-total_payed);
            }
            for( auto bet : gr_range.true_bets ) { 
               gr_range_bet_loose_operation vop;
               vop.gr_range_bet = gr_range.id;
               vop.team = gr_range.team;
               vop.lower_rank = gr_range.lower_rank;
               vop.upper_rank = gr_range.upper_rank;
               vop.result = place;
               vop.bettor = bet.first;
               push_applied_operation( vop );               
            }
         }
      } 
      else {
         for( auto bet : gr_range.true_bets ) {

            gr_range_bet_cancel_operation vop;

            vop.gr_range_bet = gr_range.id;
            vop.team = gr_range.team;
            vop.lower_rank = gr_range.lower_rank;
            vop.upper_rank = gr_range.upper_rank;
            vop.result = place;
            vop.payback = asset( bet.second.value, asset_id_type(0) );
            vop.bettor = bet.first;               
            push_applied_operation( vop );

            adjust_balance(bet.first, asset( bet.second.value, asset_id_type(0)));
         }

         for( auto bet : gr_range.false_bets ) {

            gr_range_bet_cancel_operation vop;

            vop.gr_range_bet = gr_range.id;
            vop.team = gr_range.team;
            vop.lower_rank = gr_range.lower_rank;
            vop.upper_rank = gr_range.upper_rank;
            vop.result = place;
            vop.payback = asset( bet.second.value, asset_id_type(0) );
            vop.bettor = bet.first;               
            push_applied_operation( vop );

            adjust_balance(bet.first, asset( bet.second.value, asset_id_type(0)));
         } 
      }

      ++gr_range_itr;
   }

   // REMOVIND RANGE BETS OBJECS
   while( !gr_range_bets_idx.empty())
   {
      const gr_range_bet_object& gr_range = *gr_range_bets_idx.begin();
      remove(gr_range);
   }

   // GR TEAMS BETS Proceed
   const auto& gr_team_bets_idx = get_index_type< gr_team_bet_index >().indices().get< by_id >();

   auto gr_team_itr = gr_team_bets_idx.begin();
   while( gr_team_itr != gr_team_bets_idx.end() )
   {
      const gr_team_bet_object& gr_team = *gr_team_itr;
      uint64_t team1_place = rating[gr_team.team1];
      uint64_t team2_place = rating[gr_team.team2];

      if (team1_place < team2_place) { 
         share_type total_payed=0;      
         if (gr_team.team1_bets.size()>0 && gr_team.team2_bets.size()>0) {     
            for( auto bet : gr_team.team1_bets ) {
               fc::uint128 prize_part_calc(gr_team.total_prize.value);
               prize_part_calc *= bet.second.value;
               prize_part_calc /= gr_team.total_team1_bets.value;
               uint64_t prize_part = prize_part_calc.to_uint64();
               total_payed+=prize_part;
               if (prize_part>0) {
                  gr_team_bet_win_operation vop;
                  vop.gr_team_bet = gr_team.id;
                  vop.team1 = gr_team.team1;
                  vop.team2 = gr_team.team2;
                  vop.winner = gr_team.team1;
                  vop.total_bets = asset( gr_team.total_prize, asset_id_type(0) );
                  vop.total_wins = asset( gr_team.total_team1_bets, asset_id_type(0) );
                  vop.bettor_part = asset( bet.second, asset_id_type(0) );
                  vop.reward = asset( prize_part, asset_id_type(0) );
                  vop.bettor = bet.first;
                  push_applied_operation( vop );
                  adjust_balance(bet.first, asset( prize_part, asset_id_type(0)));
               }
            }
            if (gr_team.total_prize>total_payed) {
               accumulated_fee+=(gr_team.total_prize-total_payed);
            }
            for( auto bet : gr_team.team2_bets ) { 
               gr_team_bet_loose_operation vop;
               vop.gr_team_bet = gr_team.id;
               vop.team1 = gr_team.team1;
               vop.team2 = gr_team.team2;
               vop.winner = gr_team.team1;
               vop.bettor = bet.first;
               push_applied_operation( vop );               
            }
         }
         else {
            for( auto bet : gr_team.team1_bets ) {

               gr_team_bet_cancel_operation vop;
               vop.gr_team_bet = gr_team.id;
               vop.team1 = gr_team.team1;
               vop.team2 = gr_team.team2;
               vop.winner = gr_team.team1;
               vop.payback = asset( bet.second.value, asset_id_type(0) );
               vop.bettor = bet.first;
               push_applied_operation( vop );

               adjust_balance(bet.first, asset( bet.second.value, asset_id_type(0)));
            }            
            for( auto bet : gr_team.team2_bets ) {

               gr_team_bet_cancel_operation vop;
               vop.gr_team_bet = gr_team.id;
               vop.team1 = gr_team.team1;
               vop.team2 = gr_team.team2;
               vop.winner = gr_team.team1;
               vop.payback = asset( bet.second.value, asset_id_type(0) );
               vop.bettor = bet.first;
               push_applied_operation( vop );

               adjust_balance(bet.first, asset( bet.second.value, asset_id_type(0)));
            }    
         }
      }
      else {
         share_type total_payed=0;
         if (gr_team.team1_bets.size()>0 && gr_team.team2_bets.size()>0) {             
            for( auto bet : gr_team.team2_bets ) {
               fc::uint128 prize_part_calc(gr_team.total_prize.value);
               prize_part_calc *= bet.second.value;
               prize_part_calc /= gr_team.total_team2_bets.value;
               uint64_t prize_part = prize_part_calc.to_uint64();
               total_payed+=prize_part;
               if (prize_part>0) {
                  gr_team_bet_win_operation vop;
                  vop.gr_team_bet = gr_team.id;
                  vop.team1 = gr_team.team1;
                  vop.team2 = gr_team.team2;
                  vop.winner = gr_team.team2;
                  vop.total_bets = asset( gr_team.total_prize, asset_id_type(0) );
                  vop.total_wins = asset( gr_team.total_team2_bets, asset_id_type(0) );
                  vop.bettor_part = asset( bet.second, asset_id_type(0) );
                  vop.reward = asset( prize_part, asset_id_type(0) );
                  vop.bettor = bet.first;

                  push_applied_operation( vop );
                  adjust_balance(bet.first, asset( prize_part, asset_id_type(0)));
               }
            }
            if (gr_team.total_prize>total_payed) {
               accumulated_fee+=(gr_team.total_prize-total_payed);
            }
            for( auto bet : gr_team.team1_bets ) { 
               gr_team_bet_loose_operation vop;
               vop.gr_team_bet = gr_team.id;
               vop.team1 = gr_team.team1;
               vop.team2 = gr_team.team2;
               vop.winner = gr_team.team2;
               vop.bettor = bet.first;

               push_applied_operation( vop );               
            }
         }
         else {
            for( auto bet : gr_team.team1_bets ) {

               gr_team_bet_cancel_operation vop;
               vop.gr_team_bet = gr_team.id;
               vop.team1 = gr_team.team1;
               vop.team2 = gr_team.team2;
               vop.winner = gr_team.team2;
               vop.payback = asset( bet.second.value, asset_id_type(0) );
               vop.bettor = bet.first;
               push_applied_operation( vop );

               adjust_balance(bet.first, asset( bet.second.value, asset_id_type(0)));
            }
            for( auto bet : gr_team.team2_bets ) {

               gr_team_bet_cancel_operation vop;
               vop.gr_team_bet = gr_team.id;
               vop.team1 = gr_team.team1;
               vop.team2 = gr_team.team2;
               vop.winner = gr_team.team2;
               vop.payback = asset( bet.second.value, asset_id_type(0) );
               vop.bettor = bet.first;
               push_applied_operation( vop );

               adjust_balance(bet.first, asset( bet.second.value, asset_id_type(0)));
            }            
         }
      }
      
      ++gr_team_itr;
   }
   while( !gr_team_bets_idx.empty())
   {
      const gr_team_bet_object& gr_team = *gr_team_bets_idx.begin();
      remove(gr_team);
   }

   modify(get(asset_dynamic_data_id_type()), [accumulated_fee](asset_dynamic_data_object &addo) {
      addo.accumulated_fees += accumulated_fee;
   });
}

void database::clear_gr_invite() {
   const auto& gr_invite_idx = get_index_type< gr_invite_index >().indices().get< by_id >();
   while( !gr_invite_idx.empty() )
   {
      const gr_invite_object& gr_invite = *gr_invite_idx.begin();
      remove(gr_invite);
   }
}

void database::reset_gr_rank() {
   const auto& accs_idx = get_index_type< account_index >().indices().get< by_gr_rank >();
   auto accs_itr = accs_idx.lower_bound( 1 );

   while( accs_itr != accs_idx.end() )
   {
      const account_object& acc = *accs_itr;
      ++accs_itr;

      modify( acc, []( account_object& a )
      {
         a.last_gr_rank=0;
      });
   }
   
   const auto& team_idx = get_index_type< gr_team_index >().indices().get< by_last_gr_rank >();
   auto team_itr = team_idx.lower_bound( 1 );

   while( team_itr != team_idx.end() )
   {
      const gr_team_object& team = *team_itr;
      ++team_itr;

      modify( team, []( gr_team_object& t )
      {
         t.last_gr_rank=0;
      });
   }
}

void database::assign_gr_rank_to_team(const gr_team_object& team_obj, const share_type& reward, const uint8_t& rank) {
    modify(team_obj, [rank](gr_team_object& t)
    {
        t.last_gr_rank = rank;
    });
    modify( get(team_obj.captain), [rank](account_object& a) {
        a.last_gr_rank = rank;
    });
    gr_pay_rank_reward_operation  vop;
    vop.captain = team_obj.captain;
    vop.team = team_obj.id;
    vop.amount =  asset( reward, asset_id_type(0) );
    vop.rank = rank;
    push_applied_operation( vop );
    adjust_balance(team_obj.captain, asset( reward, asset_id_type(0) ));

    for( auto player : team_obj.players ) {
        modify( get(player), [rank](account_object& a) {
            a.last_gr_rank = rank;
        });
        gr_assign_rank_operation vop;
        vop.player = player;
        vop.team = team_obj.id;
        vop.rank = rank;
        push_applied_operation( vop );
    }
}

share_type database::assign_gr_rank(const share_type& start_itr, const share_type& end_itr, const share_type& reward, const uint8_t& rank) {
    share_type result = 0;
    const dynamic_global_property_object& dgpo = get_dynamic_global_properties();

    if (dgpo.current_gr_interval == 7) {
        auto& rank_idx = get_index_type<gr_team_index>().indices().get<by_total_first_half_volume>();
        auto itr = rank_idx.lower_bound(start_itr);
        auto end = rank_idx.lower_bound(end_itr);
        if (rank == 7) {
            end = rank_idx.end();
        }
        while( itr != end )
        {
            const gr_team_object& team_obj = *itr;
            assign_gr_rank_to_team(team_obj, reward, rank);
            result += reward;
            itr++;
        }
    }

    if (dgpo.current_gr_interval == 14) {
        auto& rank_idx = get_index_type<gr_team_index>().indices().get<by_total_second_half_volume>();
        auto itr = rank_idx.lower_bound(start_itr);
        auto end = rank_idx.lower_bound(end_itr);
        if (rank == 7) {
            end = rank_idx.end();
        }
        while( itr != end )
        {
            const gr_team_object& team_obj = *itr;
            assign_gr_rank_to_team(team_obj, reward, rank);
            result += reward;
            itr++;
        }
    } 
    return result;
}


void database::proceed_gr_rank() {
   reset_gr_rank();
   const dynamic_global_property_object& dgpo = get_dynamic_global_properties();

   share_type total_reward = 0;
   // IRON
    total_reward += assign_gr_rank(dgpo.gr_iron_volume, dgpo.gr_bronze_volume-int64_t(1), dgpo.gr_iron_reward, 1);
   // BRONZE
    total_reward += assign_gr_rank(dgpo.gr_bronze_volume, dgpo.gr_silver_volume-int64_t(1), dgpo.gr_bronze_reward, 2);
   // SILVER
    total_reward += assign_gr_rank(dgpo.gr_silver_volume, dgpo.gr_gold_volume-int64_t(1), dgpo.gr_silver_reward, 3);
   // GOLD
    total_reward += assign_gr_rank(dgpo.gr_gold_volume, dgpo.gr_platinum_volume-int64_t(1), dgpo.gr_gold_reward, 4);
   // PLATINUM
    total_reward += assign_gr_rank(dgpo.gr_platinum_volume, dgpo.gr_diamond_volume-int64_t(1), dgpo.gr_platinum_reward, 5);
   // DIAMOND
    total_reward += assign_gr_rank(dgpo.gr_diamond_volume, dgpo.gr_master_volume-int64_t(1), dgpo.gr_diamond_reward, 6);
   // MASTER
    total_reward += assign_gr_rank(dgpo.gr_master_volume, 0, dgpo.gr_master_reward, 7);

   // ELITE

    if (dgpo.current_gr_interval == 7) {
        auto& rank_idx = get_index_type<gr_team_index>().indices().get<by_total_first_half_volume>();
    if (rank_idx.size()>=10) {
            auto itr = rank_idx.rbegin();
            for(int i = 0; i < 10; i++) {
                const gr_team_object& team_obj = *itr;
                if (team_obj.last_gr_rank == 7) {
                    assign_gr_rank_to_team(team_obj, dgpo.gr_elite_reward, 8);
                    total_reward += dgpo.gr_elite_reward;
                }
                itr++;
            }
        }
    }
    if (dgpo.current_gr_interval == 14) {
  
        auto& rank_idx = get_index_type<gr_team_index>().indices().get<by_total_second_half_volume>();
    if (rank_idx.size()>=10) {
            auto itr = rank_idx.rbegin();
            for(int i = 0; i < 10; i++) {
                const gr_team_object& team_obj = *itr;
                if (team_obj.last_gr_rank == 7) {
                    assign_gr_rank_to_team(team_obj, dgpo.gr_elite_reward, 8);
                    total_reward += dgpo.gr_elite_reward;
                }
                itr++;
            }
        }
    }

   // DINAMIC ASSET DATA
   modify( get_core_dynamic_data(), [total_reward](asset_dynamic_data_object& d) {
      d.current_supply += total_reward;
   });
};

void database::init_gr_race() {
// FIRST APOSTOLOS
// timy4en 1.2.32
// billionare-win 1.2.35
// art-unicorn 1.2.37
// investor-kmm 1.2.66
// super-box 1.2.105
// niko-1987 1.2.176
// sveta-elina 1.2.429
// ksubagirova1 1.2.733
// yana-italyana 1.2.3559

   flat_set<account_id_type> apostolos_auth;
   apostolos_auth.insert(account_id_type(32));
   apostolos_auth.insert(account_id_type(35));
   apostolos_auth.insert(account_id_type(37));
   apostolos_auth.insert(account_id_type(66));
   apostolos_auth.insert(account_id_type(105));
   apostolos_auth.insert(account_id_type(176));
   apostolos_auth.insert(account_id_type(429));
   apostolos_auth.insert(account_id_type(733));
   apostolos_auth.insert(account_id_type(3559));

   for( const auto&  apostolos : apostolos_auth ) {
      modify( get(apostolos), [](account_object& a) {
         a.apostolos = true;
      });
   }   

   const account_object& apostolos_account = get(GRAPHENE_APOSTOLOS_ACCOUNT);
   modify( apostolos_account, [apostolos_auth](account_object& a)
   {
      a.active.weight_threshold = 0;
      a.active.clear();

      for( const auto& apostol_auth : apostolos_auth )
         {
            a.active.account_auths[apostol_auth] = 1;
            a.active.weight_threshold += 1;
         }
      a.active.weight_threshold /= 2;
      a.active.weight_threshold += 1;
   }); 
}

void database::proceed_apostolos() {

         const auto& accs_idx = get_index_type< account_index >().indices().get< by_apostolos >();
         auto accs_itr = accs_idx.lower_bound( true );

         while( accs_itr != accs_idx.end() )
         {
            const account_object& acc = *accs_itr;
            ++accs_itr;

            modify( acc, []( account_object& a )
            {
               a.apostolos=false;
            });
         }

      auto& apostolos_idx = get_index_type<gr_team_index>().indices().get<by_total_volume>();
      if (apostolos_idx.size() > 0) {
         auto team_itr = apostolos_idx.rbegin();
         const gr_team_object& apostolos_team = *team_itr;

         flat_set<account_id_type> apostolos_auth;
         apostolos_auth.insert(apostolos_team.captain);

         modify( get(apostolos_team.captain), [](account_object& a) {
            a.apostolos = true;
         });
         gr_apostolos_operation vop_cap;
         vop_cap.team = apostolos_team.id;
         vop_cap.player = apostolos_team.captain;
         push_applied_operation( vop_cap );

         for( auto apostolos : apostolos_team.players ) {
            modify( get(apostolos), [](account_object& a) {
               a.apostolos = true;
            });
            gr_apostolos_operation vop;
            vop.team = apostolos_team.id;
            vop.player = apostolos;
            push_applied_operation( vop );
            apostolos_auth.insert(apostolos);
         }      

         if( head_block_time() >= HARDFORK_CWD7_TIME ) {
            const account_object& apostolos_account = get(GRAPHENE_APOSTOLOS_ACCOUNT);
            modify( apostolos_account, [apostolos_auth](account_object& a)
            {
               a.active.weight_threshold = 0;
               a.active.clear();

               for( const auto& apostol_auth : apostolos_auth )
                  {
                     a.active.account_auths[apostol_auth] = 1;
                     a.active.weight_threshold += 1;
                  }

               a.active.weight_threshold /= 2;
               a.active.weight_threshold += 1;
            });    
         }
      }
};

void database::reset_gr_volumes() {
   const auto& team_idx = get_index_type< gr_team_index >().indices().get< by_total_volume >();
   auto team_itr = team_idx.lower_bound( 1 );

   while( team_itr != team_idx.end() )
   {
      const gr_team_object& team = *team_itr;
      ++team_itr;

      modify( team, []( gr_team_object& t )
      {
         t.gr_interval_2_volume = 0;
         t.gr_interval_4_volume = 0;
         t.gr_interval_6_volume = 0;
         t.gr_interval_9_volume = 0;
         t.gr_interval_11_volume = 0;
         t.gr_interval_13_volume = 0;
         t.first_half_volume = 0;
         t.second_half_volume = 0;
         t.total_volume = 0;
      });
   }
};

void database::count_poc_votes() {
   ilog("======================== COUNT POC VOTES ========================");
   const auto& stats_idx = get_index_type< account_stats_index >().indices().get< by_poc_vote >();
   auto stats_itr = stats_idx.lower_bound( true );
   const global_property_object& gpo = get_global_properties();

   vector<share_type> poc3_votes;
   vector<share_type> poc6_votes;
   vector<share_type> poc12_votes;

   while( stats_itr != stats_idx.end() )
   {
      const account_statistics_object& acc_stat = *stats_itr;
      ++stats_itr;
      if (acc_stat.poc3_vote>0) {
         poc3_votes.emplace_back(acc_stat.poc3_vote);
      }
      if (acc_stat.poc6_vote>0) {
         poc6_votes.emplace_back(acc_stat.poc6_vote);
      }
      if (acc_stat.poc12_vote>0) {
         poc12_votes.emplace_back(acc_stat.poc12_vote);
      }
      modify(acc_stat, [](account_statistics_object& clear_stat)
      {
         clear_stat.poc3_vote = 0;
         clear_stat.poc6_vote = 0;
         clear_stat.poc12_vote = 0;
      });
   }
   if (poc3_votes.size()>=gpo.staking_parameters.poc_min_votes) {
      std::sort(poc3_votes.begin(), poc3_votes.end());
      auto poc3_length = poc3_votes.size();
      auto poc3_filter = poc3_length*gpo.staking_parameters.poc_filter_percent/GRAPHENE_100_PERCENT;
      vector<share_type> poc3_votes_filtered(poc3_votes.begin()+poc3_filter, poc3_votes.end()-poc3_filter);
      share_type poc3_vote_sum = 0;
      for( auto poc3_vote : poc3_votes_filtered ) {
         poc3_vote_sum+=poc3_vote;
      }

      fc::uint128 poc3_sum(poc3_vote_sum.value);
      poc3_sum /= poc3_votes_filtered.size();
      poc3_sum /= GRAPHENE_BLOCKCHAIN_PRECISION;
      poc3_sum *= GRAPHENE_1_PERCENT;
      uint64_t poc3_percent = poc3_sum.to_uint64();

      ilog("====== PoC3 Sum ${s}, count ${c}, result ${r}", ("s", poc3_vote_sum)("c", poc3_votes_filtered.size())("r", poc3_percent));

      const dynamic_global_property_object& dgpo = get_dynamic_global_properties();
      modify(dgpo, [poc3_percent](dynamic_global_property_object& d) {
         d.poc3_percent = poc3_percent;
      });
   }

   if (poc6_votes.size()>=gpo.staking_parameters.poc_min_votes) {
      std::sort(poc6_votes.begin(), poc6_votes.end());
      auto poc6_length = poc6_votes.size();
      auto poc6_filter = poc6_length*gpo.staking_parameters.poc_filter_percent/GRAPHENE_100_PERCENT;
      vector<share_type> poc6_votes_filtered(poc6_votes.begin()+poc6_filter, poc6_votes.end()-poc6_filter);
      share_type poc6_vote_sum = 0;
      for( auto poc6_vote : poc6_votes_filtered ) {
         poc6_vote_sum+=poc6_vote;
      }

      fc::uint128 poc6_sum(poc6_vote_sum.value);
      poc6_sum /= poc6_votes_filtered.size();
      poc6_sum /= GRAPHENE_BLOCKCHAIN_PRECISION;
      poc6_sum *= GRAPHENE_1_PERCENT;
      uint64_t poc6_percent = poc6_sum.to_uint64();

      ilog("====== PoC6 Sum ${s}, count ${c}, result ${r}", ("s", poc6_vote_sum)("c", poc6_votes_filtered.size())("r", poc6_percent));

      const dynamic_global_property_object& dgpo = get_dynamic_global_properties();
      modify(dgpo, [poc6_percent](dynamic_global_property_object& d) {
         d.poc6_percent = poc6_percent;
      });
   }
   if (poc12_votes.size()>=gpo.staking_parameters.poc_min_votes) {
      std::sort(poc12_votes.begin(), poc12_votes.end());
      auto poc12_length = poc12_votes.size();
      auto poc12_filter = poc12_length*gpo.staking_parameters.poc_filter_percent/GRAPHENE_100_PERCENT;
      vector<share_type> poc12_votes_filtered(poc12_votes.begin()+poc12_filter, poc12_votes.end()-poc12_filter);
      share_type poc12_vote_sum = 0;
      for( auto poc12_vote : poc12_votes_filtered ) {
         poc12_vote_sum+=poc12_vote;
      }

      fc::uint128 poc12_sum(poc12_vote_sum.value);
      poc12_sum /= poc12_votes_filtered.size();
      poc12_sum /= GRAPHENE_BLOCKCHAIN_PRECISION;
      poc12_sum *= GRAPHENE_1_PERCENT;
      uint64_t poc12_percent = poc12_sum.to_uint64();

      ilog("====== PoC12 Sum ${s}, count ${c}, result ${r}", ("s", poc12_vote_sum)("c", poc12_votes_filtered.size())("r", poc12_percent));

      const dynamic_global_property_object& dgpo = get_dynamic_global_properties();
      modify(dgpo, [poc12_percent](dynamic_global_property_object& d) {
         d.poc12_percent = poc12_percent;
      });
   }

};
/// @brief A visitor for @ref worker_type which calls pay_worker on the worker within
struct worker_pay_visitor
{
   private:
      share_type pay;
      database& db;

   public:
      worker_pay_visitor(share_type pay, database& db)
         : pay(pay), db(db) {}

      typedef void result_type;
      template<typename W>
      void operator()(W& worker)const
      {
         worker.pay_worker(pay, db);
      }
};

void database::update_worker_votes()
{
   const auto& idx = get_index_type<worker_index>().indices().get<by_account>();
   auto itr = idx.begin();
   auto itr_end = idx.end();
   bool allow_negative_votes = (head_block_time() < HARDFORK_607_TIME);
   while( itr != itr_end )
   {
      modify( *itr, [this,allow_negative_votes]( worker_object& obj )
      {
         obj.total_votes_for = _vote_tally_buffer[obj.vote_for];
         obj.total_votes_against = allow_negative_votes ? _vote_tally_buffer[obj.vote_against] : 0;
      });
      ++itr;
   }
}

void database::pay_workers( share_type& budget )
{
   const auto head_time = head_block_time();
//   ilog("Processing payroll! Available budget is ${b}", ("b", budget));
   vector<std::reference_wrapper<const worker_object>> active_workers;
   // TODO optimization: add by_expiration index to avoid iterating through all objects
   get_index_type<worker_index>().inspect_all_objects([head_time, &active_workers](const object& o) {
      const worker_object& w = static_cast<const worker_object&>(o);
      if( w.is_active(head_time) && w.approving_stake() > 0 )
         active_workers.emplace_back(w);
   });

   // worker with more votes is preferred
   // if two workers exactly tie for votes, worker with lower ID is preferred
   std::sort(active_workers.begin(), active_workers.end(), [](const worker_object& wa, const worker_object& wb) {
      share_type wa_vote = wa.approving_stake();
      share_type wb_vote = wb.approving_stake();
      if( wa_vote != wb_vote )
         return wa_vote > wb_vote;
      return wa.id < wb.id;
   });

   const auto last_budget_time = get_dynamic_global_properties().last_budget_time;
   const auto passed_time_ms = head_time - last_budget_time;
   const auto passed_time_count = passed_time_ms.count();
   const auto day_count = fc::days(1).count();
   for( uint32_t i = 0; i < active_workers.size() && budget > 0; ++i )
   {
      const worker_object& active_worker = active_workers[i];
      share_type requested_pay = active_worker.daily_pay;

      // Note: if there is a good chance that passed_time_count == day_count,
      //       for better performance, can avoid the 128 bit calculation by adding a check.
      //       Since it's not the case on CrowdWiz mainnet, we're not using a check here.
      fc::uint128 pay(requested_pay.value);
      pay *= passed_time_count;
      pay /= day_count;
      requested_pay = pay.to_uint64();

      share_type actual_pay = std::min(budget, requested_pay);
      //ilog(" ==> Paying ${a} to worker ${w}", ("w", active_worker.id)("a", actual_pay));
      modify(active_worker, [&](worker_object& w) {
         w.worker.visit(worker_pay_visitor(actual_pay, *this));
      });

      budget -= actual_pay;
   }
}

void database::update_active_witnesses()
{ try {
   assert( _witness_count_histogram_buffer.size() > 0 );
   share_type stake_target = (_total_voting_stake-_witness_count_histogram_buffer[0]) / 2;

   /// accounts that vote for 0 or 1 witness do not get to express an opinion on
   /// the number of witnesses to have (they abstain and are non-voting accounts)

   share_type stake_tally = 0; 

   size_t witness_count = 0;
   if( stake_target > 0 )
   {
      while( (witness_count < _witness_count_histogram_buffer.size() - 1)
             && (stake_tally <= stake_target) )
      {
         stake_tally += _witness_count_histogram_buffer[++witness_count];
      }
   }

   const chain_property_object& cpo = get_chain_properties();

   witness_count = std::max( witness_count*2+1, (size_t)cpo.immutable_parameters.min_witness_count );
   auto wits = sort_votable_objects<witness_index>( witness_count );

   const global_property_object& gpo = get_global_properties();

   auto update_witness_total_votes = [this]( const witness_object& wit ) {
      modify( wit, [this]( witness_object& obj )
      {
         obj.total_votes = _vote_tally_buffer[obj.vote_id];
      });
   };

   if( _track_standby_votes )
   {
      const auto& all_witnesses = get_index_type<witness_index>().indices();
      for( const witness_object& wit : all_witnesses )
      {
         update_witness_total_votes( wit );
      }
   }
   else
   {
      for( const witness_object& wit : wits )
      {
         update_witness_total_votes( wit );
      }
   }

   // Update witness authority
   modify( get(GRAPHENE_WITNESS_ACCOUNT), [this,&wits]( account_object& a )
   {
      if( head_block_time() < HARDFORK_533_TIME )
      {
         uint64_t total_votes = 0;
         map<account_id_type, uint64_t> weights;
         a.active.weight_threshold = 0;
         a.active.clear();

         for( const witness_object& wit : wits )
         {
            weights.emplace(wit.witness_account, _vote_tally_buffer[wit.vote_id]);
            total_votes += _vote_tally_buffer[wit.vote_id];
         }

         // total_votes is 64 bits. Subtract the number of leading low bits from 64 to get the number of useful bits,
         // then I want to keep the most significant 16 bits of what's left.
         int8_t bits_to_drop = std::max(int(boost::multiprecision::detail::find_msb(total_votes)) - 15, 0);
         for( const auto& weight : weights )
         {
            // Ensure that everyone has at least one vote. Zero weights aren't allowed.
            uint16_t votes = std::max((weight.second >> bits_to_drop), uint64_t(1) );
            a.active.account_auths[weight.first] += votes;
            a.active.weight_threshold += votes;
         }

         a.active.weight_threshold /= 2;
         a.active.weight_threshold += 1;
      }
      else
      {
         vote_counter vc;
         for( const witness_object& wit : wits )
            vc.add( wit.witness_account, _vote_tally_buffer[wit.vote_id] );
         vc.finish( a.active );
      }
   } );

   modify( gpo, [&wits]( global_property_object& gp )
   {
      gp.active_witnesses.clear();
      gp.active_witnesses.reserve(wits.size());
      std::transform(wits.begin(), wits.end(),
                     std::inserter(gp.active_witnesses, gp.active_witnesses.end()),
                     [](const witness_object& w) {
         return w.id;
      });
   });

} FC_CAPTURE_AND_RETHROW() }

void database::update_active_committee_members()
{ try {
   assert( _committee_count_histogram_buffer.size() > 0 );
   share_type stake_target = (_total_voting_stake-_committee_count_histogram_buffer[0]) / 2;

   /// accounts that vote for 0 or 1 witness do not get to express an opinion on
   /// the number of witnesses to have (they abstain and are non-voting accounts)
   uint64_t stake_tally = 0; // _committee_count_histogram_buffer[0];
   size_t committee_member_count = 0;
   if( stake_target > 0 )
   {
      while( (committee_member_count < _committee_count_histogram_buffer.size() - 1)
             && (stake_tally <= stake_target) )
      {
         stake_tally += _committee_count_histogram_buffer[++committee_member_count];
      }
   }

   const chain_property_object& cpo = get_chain_properties();

   committee_member_count = std::max( committee_member_count*2+1, (size_t)cpo.immutable_parameters.min_committee_member_count );
   auto committee_members = sort_votable_objects<committee_member_index>( committee_member_count );

   auto update_committee_member_total_votes = [this]( const committee_member_object& cm ) {
      modify( cm, [this]( committee_member_object& obj )
      {
         obj.total_votes = _vote_tally_buffer[obj.vote_id];
      });
   };

   if( _track_standby_votes )
   {
      const auto& all_committee_members = get_index_type<committee_member_index>().indices();
      for( const committee_member_object& cm : all_committee_members )
      {
         update_committee_member_total_votes( cm );
      }
   }
   else
   {
      for( const committee_member_object& cm : committee_members )
      {
         update_committee_member_total_votes( cm );
      }
   }

   // Update committee authorities
   if( !committee_members.empty() )
   {
      const account_object& committee_account = get(GRAPHENE_COMMITTEE_ACCOUNT);
      modify( committee_account, [this,&committee_members](account_object& a)
      {
         if( head_block_time() < HARDFORK_533_TIME )
         {
            uint64_t total_votes = 0;
            map<account_id_type, uint64_t> weights;
            a.active.weight_threshold = 0;
            a.active.clear();

            for( const committee_member_object& cm : committee_members )
            {
               weights.emplace( cm.committee_member_account, _vote_tally_buffer[cm.vote_id] );
               total_votes += _vote_tally_buffer[cm.vote_id];
            }

            // total_votes is 64 bits. Subtract the number of leading low bits from 64 to get the number of useful bits,
            // then I want to keep the most significant 16 bits of what's left.
            int8_t bits_to_drop = std::max(int(boost::multiprecision::detail::find_msb(total_votes)) - 15, 0);
            for( const auto& weight : weights )
            {
               // Ensure that everyone has at least one vote. Zero weights aren't allowed.
               uint16_t votes = std::max((weight.second >> bits_to_drop), uint64_t(1) );
               a.active.account_auths[weight.first] += votes;
               a.active.weight_threshold += votes;
            }

            a.active.weight_threshold /= 2;
            a.active.weight_threshold += 1;
         }
         else
         {
            vote_counter vc;
            for( const committee_member_object& cm : committee_members )
               vc.add( cm.committee_member_account, _vote_tally_buffer[cm.vote_id] );
            vc.finish( a.active );
         }
      });
      modify( get(GRAPHENE_RELAXED_COMMITTEE_ACCOUNT), [&committee_account](account_object& a)
      {
         a.active = committee_account.active;
      });
   }
   modify( get_global_properties(), [&committee_members](global_property_object& gp)
   {
      gp.active_committee_members.clear();
      std::transform(committee_members.begin(), committee_members.end(),
                     std::inserter(gp.active_committee_members, gp.active_committee_members.begin()),
                     [](const committee_member_object& d) { return d.id; });
   });
} FC_CAPTURE_AND_RETHROW() }

void database::initialize_budget_record( fc::time_point_sec now, budget_record& rec )const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   const asset_object& core = get_core_asset();
   const asset_dynamic_data_object& core_dd = get_core_dynamic_data();

   rec.from_initial_reserve = core.reserved(*this);
   rec.from_accumulated_fees = core_dd.accumulated_fees;
   rec.from_unused_witness_budget = dpo.witness_budget;

   if(    (dpo.last_budget_time == fc::time_point_sec())
       || (now <= dpo.last_budget_time) )
   {
      rec.time_since_last_budget = 0;
      return;
   }

   int64_t dt = (now - dpo.last_budget_time).to_seconds();
   rec.time_since_last_budget = uint64_t( dt );

   // We'll consider accumulated_fees to be reserved at the BEGINNING
   // of the maintenance interval.  However, for speed we only
   // call modify() on the asset_dynamic_data_object once at the
   // end of the maintenance interval.  Thus the accumulated_fees
   // are available for the budget at this point, but not included
   // in core.reserved().
   share_type reserve = rec.from_initial_reserve + core_dd.accumulated_fees;
   // Similarly, we consider leftover witness_budget to be burned
   // at the BEGINNING of the maintenance interval.
   reserve += dpo.witness_budget;

   fc::uint128_t budget_u128 = reserve.value;
   budget_u128 *= uint64_t(dt);
   budget_u128 *= GRAPHENE_CORE_ASSET_CYCLE_RATE;
   //round up to the nearest satoshi -- this is necessary to ensure
   //   there isn't an "untouchable" reserve, and we will eventually
   //   be able to use the entire reserve
   budget_u128 += ((uint64_t(1) << GRAPHENE_CORE_ASSET_CYCLE_RATE_BITS) - 1);
   budget_u128 >>= GRAPHENE_CORE_ASSET_CYCLE_RATE_BITS;
   if( budget_u128 < reserve.value )
      rec.total_budget = share_type(budget_u128.to_uint64());
   else
      rec.total_budget = reserve;

   return;
}

share_type cut_fee_helper(share_type a)
{
   fc::uint128 r(a.value);
   r *= 40;
   r /= 42;
   return r.to_uint64();
}

share_type split_fee_helper(share_type a, uint16_t p)
{
   if (a == 0 || p == 0)
      return 0;

   fc::uint128 r(a.value);
   r *= p;
   r /= 21000;
   return r.to_uint64();
}

share_type split_fee_helper_dynamic(share_type a, uint16_t p, share_type current_supply)
{
   if (a == 0 || p == 0 || current_supply == 0)
      return 0;
   fc::uint128 d(current_supply.value);
   fc::uint128 r(a.value);
   r *= p;
   r /= d;
   return r.to_uint64();
}

/**
 * Split accumulated_fees from core Asset between GOLD CWD token holders
 */

void count_gold( database& db )
{
   try
   {
      const asset_dynamic_data_object& core = db.get_core_dynamic_data();

      if (core.accumulated_fees>0) {
         // ilog( "count_gold Before GOLD CASHBACK, ACCUMULATED FEES ${accumulated_fees}", ("accumulated_fees",core.accumulated_fees));
         share_type splitable_fees = cut_fee_helper(core.accumulated_fees);
         share_type payed_fees = 0;
         share_type current_gcwd_supply = 0;
         if ( db.head_block_time() >= HARDFORK_CWD2_TIME ) {
            const asset_dynamic_data_object& gcwd_dyn_data = db.get( asset_dynamic_data_id_type(1) );
            current_gcwd_supply = gcwd_dyn_data.current_supply;
         }
         const auto& bal_idx = db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
         auto range = bal_idx.equal_range( boost::make_tuple( asset_id_type(1) ) );
         for( const account_balance_object& bal : boost::make_iterator_range( range.first, range.second ) )
         {
            if( bal.balance.value == 0 )
                  continue;
            share_type reward_cut=0;
            if ( db.head_block_time() >= HARDFORK_CWD2_TIME ) {
               reward_cut=split_fee_helper_dynamic(splitable_fees, bal.balance.value, current_gcwd_supply);               
               }
            else {
               reward_cut=split_fee_helper(splitable_fees, bal.balance.value);               
            }
            payed_fees+=reward_cut;
            const auto account = db.find(bal.owner);

            if ( db.head_block_time() >= HARDFORK_CWD6_TIME ) {
               db.deposit_cashback(*account, reward_cut, false, false);
            }
            else{
               db.deposit_cashback(*account, reward_cut, false, true);
            }

            // ilog( "Deposit GOLD Cashback =${acc}= amount ${reward_cut}", ("acc",account->name)("reward_cut",reward_cut));
         }
         db.modify(core, [&]( asset_dynamic_data_object& _core )
         {
            _core.accumulated_fees = core.accumulated_fees-payed_fees;
         });
      }
   }
   FC_CAPTURE_AND_RETHROW()
}

/**
 * Update the budget for witnesses and workers.
 */
void database::process_budget()
{
   try
   {
      const global_property_object& gpo = get_global_properties();
      const dynamic_global_property_object& dpo = get_dynamic_global_properties();
      const asset_dynamic_data_object& core = get_core_dynamic_data();
      fc::time_point_sec now = head_block_time();
      int64_t time_to_maint = (dpo.next_maintenance_time - now).to_seconds();
      //
      // The code that generates the next maintenance time should
      //    only produce a result in the future.  If this assert
      //    fails, then the next maintenance time algorithm is buggy.
      //
      assert( time_to_maint > 0 );
      //
      // Code for setting chain parameters should validate
      //    block_interval > 0 (as well as the humans proposing /
      //    voting on changes to block interval).
      //
      assert( gpo.parameters.block_interval > 0 );
      uint64_t blocks_to_maint = (uint64_t(time_to_maint) + gpo.parameters.block_interval - 1) / gpo.parameters.block_interval;

      // blocks_to_maint > 0 because time_to_maint > 0,
      // which means numerator is at least equal to block_interval

      budget_record rec;
      initialize_budget_record( now, rec );
      share_type available_funds = rec.total_budget;

      share_type witness_budget = gpo.parameters.witness_pay_per_block.value * blocks_to_maint;
      rec.requested_witness_budget = witness_budget;
      witness_budget = std::min(witness_budget, available_funds);
      rec.witness_budget = witness_budget;
      available_funds -= witness_budget;

      fc::uint128_t worker_budget_u128 = gpo.parameters.worker_budget_per_day.value;
      worker_budget_u128 *= uint64_t(time_to_maint);
      worker_budget_u128 /= 60*60*24;

      share_type worker_budget;
      if( worker_budget_u128 >= available_funds.value )
         worker_budget = available_funds;
      else
         worker_budget = worker_budget_u128.to_uint64();
      rec.worker_budget = worker_budget;
      available_funds -= worker_budget;

      share_type leftover_worker_funds = worker_budget;
      pay_workers(leftover_worker_funds);
      rec.leftover_worker_funds = leftover_worker_funds;
      available_funds += leftover_worker_funds;

      rec.supply_delta = rec.witness_budget
         + rec.worker_budget
         - rec.leftover_worker_funds
         - rec.from_accumulated_fees
         - rec.from_unused_witness_budget;

      modify(core, [&]( asset_dynamic_data_object& _core )
      {
         _core.current_supply = (_core.current_supply + rec.supply_delta );

         assert( rec.supply_delta ==
                                   witness_budget
                                 + worker_budget
                                 - leftover_worker_funds
                                 - _core.accumulated_fees
                                 - dpo.witness_budget
                                );
         _core.accumulated_fees = 0;
      });

      modify(dpo, [&]( dynamic_global_property_object& _dpo )
      {
         // Since initial witness_budget was rolled into
         // available_funds, we replace it with witness_budget
         // instead of adding it.
         _dpo.witness_budget = witness_budget;
         _dpo.last_budget_time = now;
      });

      create< budget_record_object >( [&]( budget_record_object& _rec )
      {
         _rec.time = head_block_time();
         _rec.record = rec;
      });

      // available_funds is money we could spend, but don't want to.
      // we simply let it evaporate back into the reserve.
   }
   FC_CAPTURE_AND_RETHROW()
}

template< typename Visitor >
void visit_special_authorities( const database& db, Visitor visit )
{
   const auto& sa_idx = db.get_index_type< special_authority_index >().indices().get<by_id>();

   for( const special_authority_object& sao : sa_idx )
   {
      const account_object& acct = sao.account(db);
      if( acct.owner_special_authority.which() != special_authority::tag< no_special_authority >::value )
      {
         visit( acct, true, acct.owner_special_authority );
      }
      if( acct.active_special_authority.which() != special_authority::tag< no_special_authority >::value )
      {
         visit( acct, false, acct.active_special_authority );
      }
   }
}

void update_top_n_authorities( database& db )
{
   visit_special_authorities( db,
   [&]( const account_object& acct, bool is_owner, const special_authority& auth )
   {
      if( auth.which() == special_authority::tag< top_holders_special_authority >::value )
      {
         // use index to grab the top N holders of the asset and vote_counter to obtain the weights

         const top_holders_special_authority& tha = auth.get< top_holders_special_authority >();
         vote_counter vc;
         const auto& bal_idx = db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
         uint8_t num_needed = tha.num_top_holders;
         if( num_needed == 0 )
            return;

         // find accounts
         const auto range = bal_idx.equal_range( boost::make_tuple( tha.asset ) );
         for( const account_balance_object& bal : boost::make_iterator_range( range.first, range.second ) )
         {
             assert( bal.asset_type == tha.asset );
             if( bal.owner == acct.id )
                continue;
             vc.add( bal.owner, bal.balance.value );
             --num_needed;
             if( num_needed == 0 )
                break;
         }

         db.modify( acct, [&]( account_object& a )
         {
            vc.finish( is_owner ? a.owner : a.active );
            if( !vc.is_empty() )
               a.top_n_control_flags |= (is_owner ? account_object::top_n_control_owner : account_object::top_n_control_active);
         } );
      }
   } );
}

void split_fba_balance(
   database& db,
   uint64_t fba_id,
   uint16_t network_pct,
   uint16_t designated_asset_buyback_pct,
   uint16_t designated_asset_issuer_pct
)
{
   FC_ASSERT( uint32_t(network_pct) + uint32_t(designated_asset_buyback_pct) + uint32_t(designated_asset_issuer_pct) == GRAPHENE_100_PERCENT );
   const fba_accumulator_object& fba = fba_accumulator_id_type( fba_id )(db);
   if( fba.accumulated_fba_fees == 0 )
      return;

   const asset_dynamic_data_object& core_dd = db.get_core_dynamic_data();

   if( !fba.is_configured(db) )
   {
      ilog( "${n} core given to network at block ${b} due to non-configured FBA", ("n", fba.accumulated_fba_fees)("b", db.head_block_time()) );
      db.modify( core_dd, [&]( asset_dynamic_data_object& _core_dd )
      {
         _core_dd.current_supply -= fba.accumulated_fba_fees;
      } );
      db.modify( fba, [&]( fba_accumulator_object& _fba )
      {
         _fba.accumulated_fba_fees = 0;
      } );
      return;
   }

   fc::uint128_t buyback_amount_128 = fba.accumulated_fba_fees.value;
   buyback_amount_128 *= designated_asset_buyback_pct;
   buyback_amount_128 /= GRAPHENE_100_PERCENT;
   share_type buyback_amount = buyback_amount_128.to_uint64();

   fc::uint128_t issuer_amount_128 = fba.accumulated_fba_fees.value;
   issuer_amount_128 *= designated_asset_issuer_pct;
   issuer_amount_128 /= GRAPHENE_100_PERCENT;
   share_type issuer_amount = issuer_amount_128.to_uint64();

   // this assert should never fail
   FC_ASSERT( buyback_amount + issuer_amount <= fba.accumulated_fba_fees );

   share_type network_amount = fba.accumulated_fba_fees - (buyback_amount + issuer_amount);

   const asset_object& designated_asset = (*fba.designated_asset)(db);

   if( network_amount != 0 )
   {
      db.modify( core_dd, [&]( asset_dynamic_data_object& _core_dd )
      {
         _core_dd.current_supply -= network_amount;
      } );
   }

   fba_distribute_operation vop;
   vop.account_id = *designated_asset.buyback_account;
   vop.fba_id = fba.id;
   vop.amount = buyback_amount;
   if( vop.amount != 0 )
   {
      db.adjust_balance( *designated_asset.buyback_account, asset(buyback_amount) );
      db.push_applied_operation(vop);
   }

   vop.account_id = designated_asset.issuer;
   vop.fba_id = fba.id;
   vop.amount = issuer_amount;
   if( vop.amount != 0 )
   {
      db.adjust_balance( designated_asset.issuer, asset(issuer_amount) );
      db.push_applied_operation(vop);
   }

   db.modify( fba, [&]( fba_accumulator_object& _fba )
   {
      _fba.accumulated_fba_fees = 0;
   } );
}

void distribute_fba_balances( database& db )
{
   split_fba_balance( db, fba_accumulator_id_transfer_to_blind  , 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
   split_fba_balance( db, fba_accumulator_id_blind_transfer     , 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
   split_fba_balance( db, fba_accumulator_id_transfer_from_blind, 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
}

void create_buyback_orders( database& db )
{
   const auto& bbo_idx = db.get_index_type< buyback_index >().indices().get<by_id>();
   const auto& bal_idx = db.get_index_type< primary_index< account_balance_index > >().get_secondary_index< balances_by_account_index >();

   for( const buyback_object& bbo : bbo_idx )
   {
      const asset_object& asset_to_buy = bbo.asset_to_buy(db);
      assert( asset_to_buy.buyback_account.valid() );

      const account_object& buyback_account = (*(asset_to_buy.buyback_account))(db);

      if( !buyback_account.allowed_assets.valid() )
      {
         wlog( "skipping buyback account ${b} at block ${n} because allowed_assets does not exist", ("b", buyback_account)("n", db.head_block_num()) );
         continue;
      }

      for( const auto& entry : bal_idx.get_account_balances( buyback_account.id ) )
      {
         const auto* it = entry.second;
         asset_id_type asset_to_sell = it->asset_type;
         share_type amount_to_sell = it->balance;
         if( asset_to_sell == asset_to_buy.id )
            continue;
         if( amount_to_sell == 0 )
            continue;
         if( buyback_account.allowed_assets->find( asset_to_sell ) == buyback_account.allowed_assets->end() )
         {
            wlog( "buyback account ${b} not selling disallowed holdings of asset ${a} at block ${n}", ("b", buyback_account)("a", asset_to_sell)("n", db.head_block_num()) );
            continue;
         }

         try
         {
            transaction_evaluation_state buyback_context(&db);
            buyback_context.skip_fee_schedule_check = true;

            limit_order_create_operation create_vop;
            create_vop.fee = asset( 0, asset_id_type() );
            create_vop.seller = buyback_account.id;
            create_vop.amount_to_sell = asset( amount_to_sell, asset_to_sell );
            create_vop.min_to_receive = asset( 1, asset_to_buy.id );
            create_vop.expiration = time_point_sec::maximum();
            create_vop.fill_or_kill = false;

            limit_order_id_type order_id = db.apply_operation( buyback_context, create_vop ).get< object_id_type >();

            if( db.find( order_id ) != nullptr )
            {
               limit_order_cancel_operation cancel_vop;
               cancel_vop.fee = asset( 0, asset_id_type() );
               cancel_vop.order = order_id;
               cancel_vop.fee_paying_account = buyback_account.id;

               db.apply_operation( buyback_context, cancel_vop );
            }
         }
         catch( const fc::exception& e )
         {
            // we can in fact get here, e.g. if asset issuer of buy/sell asset blacklists/whitelists the buyback account
            wlog( "Skipping buyback processing selling ${as} for ${ab} for buyback account ${b} at block ${n}; exception was ${e}",
                  ("as", asset_to_sell)("ab", asset_to_buy)("b", buyback_account)("n", db.head_block_num())("e", e.to_detail_string()) );
            continue;
         }
      }
   }
   return;
}

void deprecate_annual_members( database& db )
{
   const auto& account_idx = db.get_index_type<account_index>().indices().get<by_id>();
   fc::time_point_sec now = db.head_block_time();
   for( const account_object& acct : account_idx )
   {
      try
      {
         transaction_evaluation_state upgrade_context(&db);
         upgrade_context.skip_fee_schedule_check = true;

         if( acct.is_annual_member( now ) )
         {
            account_upgrade_operation upgrade_vop;
            upgrade_vop.fee = asset( 0, asset_id_type() );
            upgrade_vop.account_to_upgrade = acct.id;
            upgrade_vop.upgrade_to_lifetime_member = true;
            db.apply_operation( upgrade_context, upgrade_vop );
         }
      }
      catch( const fc::exception& e )
      {
         // we can in fact get here, e.g. if asset issuer of buy/sell asset blacklists/whitelists the buyback account
         wlog( "Skipping annual member deprecate processing for account ${a} (${an}) at block ${n}; exception was ${e}",
               ("a", acct.id)("an", acct.name)("n", db.head_block_num())("e", e.to_detail_string()) );
         continue;
      }
   }
   return;
}

void database::process_bids( const asset_bitasset_data_object& bad )
{
   if( bad.is_prediction_market ) return;
   if( bad.current_feed.settlement_price.is_null() ) return;

   asset_id_type to_revive_id = (asset( 0, bad.options.short_backing_asset ) * bad.settlement_price).asset_id;
   const asset_object& to_revive = to_revive_id( *this );
   const asset_dynamic_data_object& bdd = to_revive.dynamic_data( *this );

   const auto& bid_idx = get_index_type< collateral_bid_index >().indices().get<by_price>();
   const auto start = bid_idx.lower_bound( boost::make_tuple( to_revive_id, price::max( bad.options.short_backing_asset, to_revive_id ), collateral_bid_id_type() ) );

   share_type covered = 0;
   auto itr = start;
   while( covered < bdd.current_supply && itr != bid_idx.end() && itr->inv_swan_price.quote.asset_id == to_revive_id )
   {
      const collateral_bid_object& bid = *itr;
      asset debt_in_bid = bid.inv_swan_price.quote;
      if( debt_in_bid.amount > bdd.current_supply )
         debt_in_bid.amount = bdd.current_supply;
      asset total_collateral = debt_in_bid * bad.settlement_price;
      total_collateral += bid.inv_swan_price.base;
      price call_price = price::call_price( debt_in_bid, total_collateral, bad.current_feed.maintenance_collateral_ratio );
      if( ~call_price >= bad.current_feed.settlement_price ) break;
      covered += debt_in_bid.amount;
      ++itr;
   }
   if( covered < bdd.current_supply ) return;

   const auto end = itr;
   share_type to_cover = bdd.current_supply;
   share_type remaining_fund = bad.settlement_fund;
   for( itr = start; itr != end; )
   {
      const collateral_bid_object& bid = *itr;
      ++itr;
      asset debt_in_bid = bid.inv_swan_price.quote;
      if( debt_in_bid.amount > bdd.current_supply )
         debt_in_bid.amount = bdd.current_supply;
      share_type debt = debt_in_bid.amount;
      share_type collateral = (debt_in_bid * bad.settlement_price).amount;
      if( debt >= to_cover )
      {
         debt = to_cover;
         collateral = remaining_fund;
      }
      to_cover -= debt;
      remaining_fund -= collateral;
      execute_bid( bid, debt, collateral, bad.current_feed );
   }
   FC_ASSERT( remaining_fund == 0 );
   FC_ASSERT( to_cover == 0 );

   _cancel_bids_and_revive_mpa( to_revive, bad );
}

void update_and_match_call_orders( database& db )
{
   // Update call_price
   wlog( "Updating all call orders for hardfork core-343 at block ${n}", ("n",db.head_block_num()) );
   asset_id_type current_asset;
   const asset_bitasset_data_object* abd = nullptr;
   // by_collateral index won't change after call_price updated, so it's safe to iterate
   for( const auto& call_obj : db.get_index_type<call_order_index>().indices().get<by_collateral>() )
   {
      if( current_asset != call_obj.debt_type() ) // debt type won't be asset_id_type(), abd will always get initialized
      {
         current_asset = call_obj.debt_type();
         abd = &current_asset(db).bitasset_data(db);
      }
      if( !abd || abd->is_prediction_market ) // nothing to do with PM's; check !abd just to be safe
         continue;
      db.modify( call_obj, [abd]( call_order_object& call ) {
         call.call_price  =  price::call_price( call.get_debt(), call.get_collateral(),
                                                abd->current_feed.maintenance_collateral_ratio );
      });
   }
   // Match call orders
   const auto& asset_idx = db.get_index_type<asset_index>().indices().get<by_type>();
   auto itr = asset_idx.lower_bound( true /** market issued */ );
   while( itr != asset_idx.end() )
   {
      const asset_object& a = *itr;
      ++itr;
      // be here, next_maintenance_time should have been updated already
      db.check_call_orders( a, true, false ); // allow black swan, and call orders are taker
   }
   wlog( "Done updating all call orders for hardfork core-343 at block ${n}", ("n",db.head_block_num()) );
}

void database::process_bitassets()
{
   time_point_sec head_time = head_block_time();
   uint32_t head_epoch_seconds = head_time.sec_since_epoch();
   bool after_hf_core_518 = ( head_time >= HARDFORK_CORE_518_TIME ); // clear expired feeds

   const auto update_bitasset = [this,head_time,head_epoch_seconds,after_hf_core_518]( asset_bitasset_data_object &o )
   {
      o.force_settled_volume = 0; // Reset all BitAsset force settlement volumes to zero

      // clear expired feeds
      if( after_hf_core_518 )
      {
         const auto &asset = get( o.asset_id );
         auto flags = asset.options.flags;
         if ( ( flags & ( witness_fed_asset | committee_fed_asset ) ) &&
              o.options.feed_lifetime_sec < head_epoch_seconds ) // if smartcoin && check overflow
         {
            fc::time_point_sec calculated = head_time - o.options.feed_lifetime_sec;
            for( auto itr = o.feeds.rbegin(); itr != o.feeds.rend(); ) // loop feeds
            {
               auto feed_time = itr->second.first;
               std::advance( itr, 1 );
               if( feed_time < calculated )
                  o.feeds.erase( itr.base() ); // delete expired feed
            }
         }
      }
   };

   for( const auto& d : get_index_type<asset_bitasset_data_index>().indices() )
   {
      modify( d, update_bitasset );
      if( d.has_settlement() )
         process_bids(d);
   }
}

/******
 * @brief one-time data process for hard fork core-868-890
 *
 * Prior to hardfork 868, switching a bitasset's shorting asset would not reset its
 * feeds. This method will run at the hardfork time, and erase (or nullify) feeds
 * that have incorrect backing assets.
 * https://github.com/bitshares/bitshares-core/issues/868
 *
 * Prior to hardfork 890, changing a bitasset's feed expiration time would not
 * trigger a median feed update. This method will run at the hardfork time, and
 * correct all median feed data.
 * https://github.com/bitshares/bitshares-core/issues/890
 *
 * @param db the database
 * @param skip_check_call_orders true if check_call_orders() should not be called
 */
// TODO: for better performance, this function can be removed if it actually updated nothing at hf time.
//       * Also need to update related test cases
//       * NOTE: the removal can't be applied to testnet
void process_hf_868_890( database& db, bool skip_check_call_orders )
{
   const auto head_time = db.head_block_time();
   const auto head_num = db.head_block_num();
   wlog( "Processing hard fork core-868-890 at block ${n}", ("n",head_num) );
   // for each market issued asset
   const auto& asset_idx = db.get_index_type<asset_index>().indices().get<by_type>();
   for( auto asset_itr = asset_idx.lower_bound(true); asset_itr != asset_idx.end(); ++asset_itr )
   {
      const auto& current_asset = *asset_itr;
      // Incorrect witness & committee feeds can simply be removed.
      // For non-witness-fed and non-committee-fed assets, set incorrect
      // feeds to price(), since we can't simply remove them. For more information:
      // https://github.com/bitshares/bitshares-core/pull/832#issuecomment-384112633
      bool is_witness_or_committee_fed = false;
      if ( current_asset.options.flags & ( witness_fed_asset | committee_fed_asset ) )
         is_witness_or_committee_fed = true;

      // for each feed
      const asset_bitasset_data_object& bitasset_data = current_asset.bitasset_data(db);
      // NOTE: We'll only need old_feed if HF343 hasn't rolled out yet
      auto old_feed = bitasset_data.current_feed;
      bool feeds_changed = false; // did any feed change
      auto itr = bitasset_data.feeds.begin();
      while( itr != bitasset_data.feeds.end() )
      {
         // If the feed is invalid
         if ( itr->second.second.settlement_price.quote.asset_id != bitasset_data.options.short_backing_asset
               && ( is_witness_or_committee_fed || itr->second.second.settlement_price != price() ) )
         {
            feeds_changed = true;
            db.modify( bitasset_data, [&itr, is_witness_or_committee_fed]( asset_bitasset_data_object& obj )
            {
               if( is_witness_or_committee_fed )
               {
                  // erase the invalid feed
                  itr = obj.feeds.erase(itr);
               }
               else
               {
                  // nullify the invalid feed
                  obj.feeds[itr->first].second.settlement_price = price();
                  ++itr;
               }
            });
         }
         else
         {
            // Feed is valid. Skip it.
            ++itr;
         }
      } // end loop of each feed

      // if any feed was modified, print a warning message
      if( feeds_changed )
      {
         wlog( "Found invalid feed for asset ${asset_sym} (${asset_id}) during hardfork core-868-890",
               ("asset_sym", current_asset.symbol)("asset_id", current_asset.id) );
      }

      // always update the median feed due to https://github.com/bitshares/bitshares-core/issues/890
      db.modify( bitasset_data, [&head_time]( asset_bitasset_data_object &obj ) {
         obj.update_median_feeds( head_time );
      });

      bool median_changed = ( old_feed.settlement_price != bitasset_data.current_feed.settlement_price );
      bool median_feed_changed = ( !( old_feed == bitasset_data.current_feed ) );
      if( median_feed_changed )
      {
         wlog( "Median feed for asset ${asset_sym} (${asset_id}) changed during hardfork core-868-890",
               ("asset_sym", current_asset.symbol)("asset_id", current_asset.id) );
      }

      // Note: due to bitshares-core issue #935, the check below (using median_changed) is incorrect.
      //       However, `skip_check_call_orders` will likely be true in both testnet and mainnet,
      //         so effectively the incorrect code won't make a difference.
      //       Additionally, we have code to update all call orders again during hardfork core-935
      // TODO cleanup after hard fork
      if( !skip_check_call_orders && median_changed ) // check_call_orders should be called
      {
         db.check_call_orders( current_asset );
      }
      else if( !skip_check_call_orders && median_feed_changed )
      {
         wlog( "Incorrectly skipped check_call_orders for asset ${asset_sym} (${asset_id}) during hardfork core-868-890",
               ("asset_sym", current_asset.symbol)("asset_id", current_asset.id) );
      }
   } // for each market issued asset
   wlog( "Done processing hard fork core-868-890 at block ${n}", ("n",head_num) );
}

/******
 * @brief one-time data process for hard fork core-935
 *
 * Prior to hardfork 935, `check_call_orders` may be unintendedly skipped when
 * median price feed has changed. This method will run at the hardfork time, and
 * call `check_call_orders` for all markets.
 * https://github.com/bitshares/bitshares-core/issues/935
 *
 * @param db the database
 */
// TODO: for better performance, this function can be removed if it actually updated nothing at hf time.
//       * Also need to update related test cases
//       * NOTE: perhaps the removal can't be applied to testnet
void process_hf_935( database& db )
{
   bool changed_something = false;
   const asset_bitasset_data_object* bitasset = nullptr;
   bool settled_before_check_call;
   bool settled_after_check_call;
   // for each market issued asset
   const auto& asset_idx = db.get_index_type<asset_index>().indices().get<by_type>();
   for( auto asset_itr = asset_idx.lower_bound(true); asset_itr != asset_idx.end(); ++asset_itr )
   {
      const auto& current_asset = *asset_itr;

      if( !changed_something )
      {
         bitasset = &current_asset.bitasset_data( db );
         settled_before_check_call = bitasset->has_settlement(); // whether already force settled
      }

      bool called_some = db.check_call_orders( current_asset );

      if( !changed_something )
      {
         settled_after_check_call = bitasset->has_settlement(); // whether already force settled

         if( settled_before_check_call != settled_after_check_call || called_some )
         {
            changed_something = true;
            wlog( "process_hf_935 changed something" );
         }
      }
   }
}

/******
 * @brief one-time data process for hardfork core 146 and hardfork core 147
 *
 * One-time burning from an account "master" 5000 CWD
 *
 * @param db the database
 */
// TODO: for better performance, this function can be removed if it actually updated nothing at hf time.
void process_hf_146_147(database& d, const signed_block& next_block)
{
   if( int(next_block.block_num()) > HARDFORK_CORE_146_BLOCK_NUM && int(next_block.block_num()) < HARDFORK_CORE_147_BLOCK_NUM )
   {
        share_type burn_amount = 500000000;
        account_id_type account_id_master = account_id_type(28);
        // burn core-asset burn_amount on master account
        d.adjust_balance(account_id_master, -asset(burn_amount, asset_id_type(0)));
        d.modify( d.get_core_dynamic_data(), [burn_amount](asset_dynamic_data_object& d) {
            d.current_supply -= burn_amount;
        });
        wlog("process_hf_146_147 changed something");
   }
}

void database::perform_chain_maintenance(const signed_block& next_block, const global_property_object& global_props)
{
   const auto& gpo = get_global_properties();

   process_hf_146_147(*this, next_block);

   distribute_fba_balances(*this);
   create_buyback_orders(*this);

   struct vote_tally_helper {
      database& d;
      const global_property_object& props;

      vote_tally_helper(database& d, const global_property_object& gpo)
         : d(d), props(gpo)
      {
         d._vote_tally_buffer.resize(props.next_available_vote_id);
         d._witness_count_histogram_buffer.resize(props.parameters.maximum_witness_count / 2 + 1);
         d._committee_count_histogram_buffer.resize(props.parameters.maximum_committee_count / 2 + 1);
         d._total_voting_stake = 0;
      }

      void operator()( const account_object& stake_account, const account_statistics_object& stats )
      {
         if( props.parameters.count_non_member_votes || stake_account.is_member(d.head_block_time()) )
         {
            // There may be a difference between the account whose stake is voting and the one specifying opinions.
            // Usually they're the same, but if the stake account has specified a voting_account, that account is the one
            // specifying the opinions.
            const account_object& opinion_account =
                  (stake_account.options.voting_account ==
                   GRAPHENE_PROXY_TO_SELF_ACCOUNT)? stake_account
                                     : d.get(stake_account.options.voting_account);

            uint64_t voting_stake = stats.total_core_in_orders.value
                  + (stake_account.cashback_vb.valid() ? (*stake_account.cashback_vb)(d).balance.amount.value: 0)
                  + stats.core_in_balance.value;

            for( vote_id_type id : opinion_account.options.votes )
            {
               uint32_t offset = id.instance();
               // if they somehow managed to specify an illegal offset, ignore it.
               if( offset < d._vote_tally_buffer.size() )
                  d._vote_tally_buffer[offset] += voting_stake;
            }

            if( opinion_account.options.num_witness <= props.parameters.maximum_witness_count )
            {
               uint16_t offset = std::min(size_t(opinion_account.options.num_witness/2),
                                          d._witness_count_histogram_buffer.size() - 1);
               // votes for a number greater than maximum_witness_count
               // are turned into votes for maximum_witness_count.
               //
               // in particular, this takes care of the case where a
               // member was voting for a high number, then the
               // parameter was lowered.
               d._witness_count_histogram_buffer[offset] += voting_stake;
            }
            if( opinion_account.options.num_committee <= props.parameters.maximum_committee_count )
            {
               uint16_t offset = std::min(size_t(opinion_account.options.num_committee/2),
                                          d._committee_count_histogram_buffer.size() - 1);
               // votes for a number greater than maximum_committee_count
               // are turned into votes for maximum_committee_count.
               //
               // same rationale as for witnesses
               d._committee_count_histogram_buffer[offset] += voting_stake;
            }

            d._total_voting_stake += voting_stake;
         }
      }
   } tally_helper(*this, gpo);

   perform_account_maintenance( tally_helper );

   struct clear_canary {
      clear_canary(vector<uint64_t>& target): target(target){}
      ~clear_canary() { target.clear(); }
   private:
      vector<uint64_t>& target;
   };
   clear_canary a(_witness_count_histogram_buffer),
                b(_committee_count_histogram_buffer),
                c(_vote_tally_buffer);

   update_top_n_authorities(*this);
   update_active_witnesses();
   update_active_committee_members();
   update_worker_votes();

   const dynamic_global_property_object& dgpo = get_dynamic_global_properties();

   modify(gpo, [&dgpo](global_property_object& p) {
      // Remove scaling of account registration fee
      p.parameters.current_fees->get<account_create_operation>().basic_fee >>= p.parameters.account_fee_scale_bitshifts *
            (dgpo.accounts_registered_this_interval / p.parameters.accounts_per_fee_scale);

      if( p.pending_parameters )
      {
         p.parameters = std::move(*p.pending_parameters);
         p.pending_parameters.reset();
      }
   });

   auto next_maintenance_time = dgpo.next_maintenance_time;
   auto maintenance_interval = gpo.parameters.maintenance_interval;

   auto next_monthly_maintenance_time = dgpo.next_monthly_maintenance_time;
   uint32_t credit_stats_interval = 2592000;
   if( next_monthly_maintenance_time <= next_block.timestamp )
   {
      if( next_block.block_num() == 1 )
         next_monthly_maintenance_time = time_point_sec() +
               (((next_block.timestamp.sec_since_epoch() / credit_stats_interval) + 1) * credit_stats_interval);
      else
      {
         auto y = (head_block_time() - next_monthly_maintenance_time).to_seconds() / credit_stats_interval;
         next_monthly_maintenance_time += (y+1) * credit_stats_interval;
      }
    perform_credit_maintenance();
    perform_p2p_maintenance();
   }
   // PoC Vote
   auto next_poc_vote_time = dgpo.next_poc_vote_time;
   auto end_poc_vote_time = dgpo.end_poc_vote_time;
   auto poc_vote_is_active = dgpo.poc_vote_is_active;
   
   if( next_poc_vote_time <= next_block.timestamp )
   {
      next_poc_vote_time = next_poc_vote_time+fc::days(gpo.staking_parameters.poc_vote_interval_days);
      end_poc_vote_time = time_point_sec() + next_block.timestamp.sec_since_epoch() + fc::seconds(gpo.staking_parameters.poc_vote_duration);
      poc_vote_is_active = true;
   }
   if( end_poc_vote_time <= next_block.timestamp and poc_vote_is_active == true ) {
      count_poc_votes();
      poc_vote_is_active = false;
   }
   // GR Intervals
   auto current_gr_interval = dgpo.current_gr_interval;
   auto next_gr_interval_time = dgpo.next_gr_interval_time;
   auto end_gr_vote_time = dgpo.end_gr_vote_time;
   auto gr_vote_is_active = dgpo.gr_vote_is_active;
   auto gr_bet_interval_time = dgpo.gr_bet_interval_time;

   const asset_object& core_asset = get_core_asset();
   const asset_dynamic_data_object& core_dyn_data = get_core_dynamic_data();

   if( next_gr_interval_time <= next_block.timestamp &&
      (next_block.timestamp < HARDFORK_CORE_144_TIME ||
      next_block.block_num() > HARDFORK_CORE_1482_BLOCK_NUM)
   )
   {
      bool hardcup_flag = ((core_dyn_data.current_supply + core_asset.options.max_supply * 0.01) > core_asset.options.max_supply);
      if (next_block.block_num() > HARDFORK_CORE_1482_BLOCK_NUM && hardcup_flag)
         wlog( "May be hardcup - Great Race process skipped!" );
      else if( current_gr_interval == 0 || current_gr_interval == 14 ) {
         if( current_gr_interval == 0 )
            init_gr_race();
         current_gr_interval = 1;
         gr_vote_is_active = true;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_1);
         end_gr_vote_time = time_point_sec() + next_block.timestamp.sec_since_epoch() + fc::seconds(gpo.greatrace_parameters.vote_duration); 
         proceed_gr_rank();
         proceed_apostolos();
         reset_gr_volumes();
         perform_gr_maintenance();
      }
      else if( current_gr_interval == 1 ) {
         current_gr_interval = 2;
         gr_bet_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_2 / uint16_t(2) );
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_2);
         clear_gr_invite();
      }
      else if( current_gr_interval == 2 ) {
         current_gr_interval = 3;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_3);
         proceed_gr_top3();
         proceed_gr_bets();
      }
      else if( current_gr_interval == 3 ) {
         current_gr_interval = 4;
         gr_bet_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_4 / uint16_t(2) );
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_4);
         clear_gr_invite();
      }
      else if( current_gr_interval == 4 ) {
         current_gr_interval = 5;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_5);
         proceed_gr_top3();
         proceed_gr_bets();
      }
      else if( current_gr_interval == 5 ) {
         current_gr_interval = 6;
         gr_bet_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_6 / uint16_t(2));
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_6);
         clear_gr_invite();
      }
      else if( current_gr_interval == 6 ) {
         current_gr_interval = 7;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_7);
         proceed_gr_top3();
         proceed_gr_bets();
      }
      else if( current_gr_interval == 7 ) {
         current_gr_interval = 8;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_8);
         proceed_gr_rank();
         perform_gr_maintenance();
      }
      else if( current_gr_interval == 8 ) {
         current_gr_interval = 9;
         gr_bet_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_9 / uint16_t(2));
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_9);
         clear_gr_invite();
      }
      else if( current_gr_interval == 9 ) {
         current_gr_interval = 10;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_10);
         proceed_gr_top3();
         proceed_gr_bets();
      }
      else if( current_gr_interval == 10 ) {
         current_gr_interval = 11;
         gr_bet_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_11 / uint16_t(2));
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_11);
         clear_gr_invite();
      }
      else if( current_gr_interval == 11 ) {
         current_gr_interval = 12;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_12);
         proceed_gr_top3();
         proceed_gr_bets();
      }
      else if( current_gr_interval == 12 ) {
         current_gr_interval = 13;
         gr_bet_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_13 / uint16_t(2));
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_13);
         clear_gr_invite();
      }
      else if( current_gr_interval == 13 ) {
         current_gr_interval = 14;
         next_gr_interval_time = next_gr_interval_time+fc::days(gpo.greatrace_parameters.interval_14);
         proceed_gr_top3();
         proceed_gr_bets();
      }
   }

   if( end_gr_vote_time <= next_block.timestamp && gr_vote_is_active == true ) {
      count_gr_votes();
      gr_vote_is_active = false;
   }  


   if( next_maintenance_time <= next_block.timestamp )
   {
      if( next_block.block_num() == 1 )
         next_maintenance_time = time_point_sec() +
               (((next_block.timestamp.sec_since_epoch() / maintenance_interval) + 1) * maintenance_interval);
      else
      {
         // We want to find the smallest k such that next_maintenance_time + k * maintenance_interval > head_block_time()
         //  This implies k > ( head_block_time() - next_maintenance_time ) / maintenance_interval
         //
         // Let y be the right-hand side of this inequality, i.e.
         // y = ( head_block_time() - next_maintenance_time ) / maintenance_interval
         //
         // and let the fractional part f be y-floor(y).  Clearly 0 <= f < 1.
         // We can rewrite f = y-floor(y) as floor(y) = y-f.
         //
         // Clearly k = floor(y)+1 has k > y as desired.  Now we must
         // show that this is the least such k, i.e. k-1 <= y.
         //
         // But k-1 = floor(y)+1-1 = floor(y) = y-f <= y.
         // So this k suffices.
         //
         auto y = (head_block_time() - next_maintenance_time).to_seconds() / maintenance_interval;
         next_maintenance_time += (y+1) * maintenance_interval;
      }
   }

   if( (dgpo.next_maintenance_time < HARDFORK_613_TIME) && (next_maintenance_time >= HARDFORK_613_TIME) )
      deprecate_annual_members(*this);

   // To reset call_price of all call orders, then match by new rule
   bool to_update_and_match_call_orders = false;
   if( (dgpo.next_maintenance_time <= HARDFORK_CORE_343_TIME) && (next_maintenance_time > HARDFORK_CORE_343_TIME) )
      to_update_and_match_call_orders = true;

   // Process inconsistent price feeds
   if( (dgpo.next_maintenance_time <= HARDFORK_CORE_868_890_TIME) && (next_maintenance_time > HARDFORK_CORE_868_890_TIME) )
      process_hf_868_890( *this, to_update_and_match_call_orders );

   // Explicitly call check_call_orders of all markets
   if( (dgpo.next_maintenance_time <= HARDFORK_CORE_935_TIME) && (next_maintenance_time > HARDFORK_CORE_935_TIME)
         && !to_update_and_match_call_orders )
   process_hf_935( *this );
   modify(dgpo, [next_maintenance_time, next_monthly_maintenance_time, next_poc_vote_time, end_poc_vote_time, poc_vote_is_active, current_gr_interval, next_gr_interval_time, end_gr_vote_time, gr_vote_is_active, gr_bet_interval_time](dynamic_global_property_object& d) {
      d.next_maintenance_time = next_maintenance_time;
      d.accounts_registered_this_interval = 0;
      d.next_monthly_maintenance_time = next_monthly_maintenance_time;
      d.next_poc_vote_time = next_poc_vote_time;
      d.end_poc_vote_time = end_poc_vote_time;
      d.poc_vote_is_active = poc_vote_is_active;
      d.current_gr_interval = current_gr_interval;
      d.next_gr_interval_time = next_gr_interval_time;
      d.end_gr_vote_time = end_gr_vote_time;
      d.gr_vote_is_active = gr_vote_is_active;
      d.gr_bet_interval_time = gr_bet_interval_time;
   });

   // We need to do it after updated next_maintenance_time, to apply new rules here
   if( to_update_and_match_call_orders )
      update_and_match_call_orders(*this);

   process_bitassets();
   //split_fees
   if(  next_maintenance_time >= HARDFORK_CWD1_TIME )
      count_gold( *this );

   // process_budget needs to run at the bottom because
   //   it needs to know the next_maintenance_time
   process_budget();
}

} }
