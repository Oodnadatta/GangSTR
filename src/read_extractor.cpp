/*
Copyright (C) 2017 Melissa Gymrek <mgymrek@ucsd.edu>
and Nima Mousavi (mousavi@ucsd.edu)

This file is part of GangSTR.

GangSTR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GangSTR is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GangSTR.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <map>
#include "src/stringops.h"
#include "src/read_extractor.h"
#include "src/realignment.h"
#include "gsl/gsl_statistics_int.h"
#include <iostream>

using namespace std;

ReadExtractor::ReadExtractor(const Options& options_) : options(options_) {
  if (options.output_readinfo) {
    readfile_.open((options.outprefix + ".readinfo.tab").c_str());
  }
}

/*
  Extracts relevant reads from bamfile and 
  populates data in the likelihood_maximizer
 */
bool ReadExtractor::ExtractReads(BamCramMultiReader* bamreader,
         const Locus& locus,
         const int32_t& regionsize,
         const int32_t& min_match, 
         LikelihoodMaximizer* likelihood_maximizer) {
  // This will keep track of information for each read pair
  std::map<std::string, ReadPair> read_pairs;
  
  if (!ProcessReadPairs(bamreader, locus, regionsize, min_match, &read_pairs)) {
    return false;
  }
  int32_t frr = 0, span = 0, encl = 0, flank = 0, offt = 0;
  if (read_pairs.size() == 0){
    // TODO print error "Not enough extracted reads"
    PrintMessageDieOnError("\tNot enough reads extracted. Aborting..", M_PROGRESS);
    return false;
  }
  // Find median bound, and filter out any bound reads with data > 3 * median
  // Find maximum bound, only pick up FRRs if max_bound > 0.2 * read_len / motif_len
  int32_t max_bound = 0, median_bound = 0, bound_thresh = 0;
  int32_t max_enclose = 0;
  std::vector<int32_t> bound_vals;
  for (std::map<std::string, ReadPair>::const_iterator iter = read_pairs.begin();
       iter != read_pairs.end(); iter++) {
    if (iter->second.read_type == RC_SPAN){
      if (iter->second.max_nCopy - 1 > 0){
	bound_vals.push_back(iter->second.max_nCopy - 1);
      }
	//if (iter->second.max_nCopy - 1 > max_bound){
	//max_bound = iter->second.max_nCopy - 1;
	//}
    } else if (iter->second.read_type == RC_BOUND){
      bound_vals.push_back(iter->second.data_value - 1);
      //if (iter->second.data_value - 1 > max_bound){
      //max_bound = iter->second.data_value -1;
      //}
    } else if (iter->second.read_type == RC_ENCL){
      if (iter->second.data_value > max_enclose){
	max_enclose = iter->second.data_value;
      }
    }
  }
  size_t bound_size = bound_vals.size();
  float read_cap = float(options.read_len) / float(locus.motif.size());
  if (bound_size == 0){
    max_bound = 0;
    median_bound = 0;
  }
  else if (bound_size == 1){
    if (bound_vals[0] > max_enclose){
      max_bound = 0;
      median_bound = 0;
    }
    else{
      max_bound = bound_vals[0];
      median_bound = bound_vals[0];
    }
  }
  else{
    sort(bound_vals.begin(), bound_vals.end());
    median_bound = bound_vals[int(bound_size / 2)];
    bound_thresh = 3 * median_bound;
    size_t idx = bound_size - 1;
    while (bound_vals[idx] > bound_thresh){
      idx--;
    }
    max_bound = bound_vals[idx];
  }
  

  bool accept_FRR;
  if (options.genome_wide){ 
    accept_FRR = ((max_enclose != 0) &&
		  (max_bound > max_enclose) && 
		  (max_bound > 0.5 * read_cap)) || 
      (max_bound > 0.8 * read_cap);
  }
  else{
    accept_FRR = ((max_enclose != 0) &&
		  (max_bound > max_enclose) && 
		  (max_bound > 0.5 * read_cap)) || 
      (max_bound > 0.8 * read_cap);
    accept_FRR = true;  // accept all FRR reads
    bound_thresh = read_cap + 5; // accept all bounding reads
  }

  /*
  cerr<<max_bound<<"\t"<<read_cap<<"\t"<<endl;
  cerr << int(accept_FRR) << endl;
  */

  /* Load data into likelihood maximizer */
  for (std::map<std::string, ReadPair>::const_iterator iter = read_pairs.begin();
       iter != read_pairs.end(); iter++) {
    /*
    if (iter->second.read_type != RC_DISCARD and iter->second.read_type != RC_UNKNOWN)
    {
      cout<<iter->first<<"\t"<<iter->second.read_type<<"\t"<<iter->second.data_value<<endl;
      cout<<((BamAlignment)iter->second.read1).QueryBases()<<endl;
      cout<<((BamAlignment)iter->second.read2).QueryBases()<<endl;
    }
    */
    if (iter->second.read_type == RC_SPAN) {
      if (iter->second.data_value < options.dist_max){
        if (options.output_readinfo) {
	  readfile_ << locus.chrom << "\t" 
		    << locus.start << "\t" 
		    << locus.end << "\t"
		    << iter->first << "\t" 
		    << "SPAN" << "\t" 
		    << iter->second.data_value << "\t" 
		    << iter->second.found_pair << std::endl;
        }
        likelihood_maximizer->AddSpanningData(iter->second.data_value);
        span++;
      }
      // In spanning case, we can also have flanking reads:
      if (iter->second.max_nCopy > 0 and iter->second.max_nCopy < bound_thresh) {
	if (options.output_readinfo) {
	  readfile_ << locus.chrom << "\t" 
		    << locus.start << "\t" 
		    << locus.end << "\t"
		    << iter->first << "\t" 
		    << "SPFLNK" << "\t" 
		    << iter->second.max_nCopy - 1 << "\t" 
		    << iter->second.found_pair << std::endl;
  }
        likelihood_maximizer->AddFlankingData(iter->second.max_nCopy-1); //-1 because flanking is always picked up +1
        flank++;
      }
    } else if (iter->second.read_type == RC_ENCL) {
      if (options.output_readinfo) {
	readfile_ << locus.chrom << "\t" 
		  << locus.start << "\t" 
		  << locus.end << "\t"
		  << iter->first << "\t" 
		  << "ENCLOSE" << "\t" 
		  << iter->second.data_value << "\t" 
		  << iter->second.found_pair << std::endl;
      }
      likelihood_maximizer->AddEnclosingData(iter->second.data_value);
      encl++;
    } else if (iter->second.read_type == RC_FRR) {
      if (accept_FRR && iter->second.data_value < options.dist_max - options.read_len){
	if (options.output_readinfo) {
	  readfile_ << locus.chrom << "\t" 
		    << locus.start << "\t" 
		    << locus.end << "\t"
		    << iter->first << "\t" 
		    << "FRR" << "\t" 
		    << iter->second.data_value << "\t" 
		    << iter->second.found_pair << std::endl;
	}
	likelihood_maximizer->AddFRRData(iter->second.data_value);
	frr++;
      }
    } else if (iter->second.read_type == RC_BOUND and iter->second.data_value < bound_thresh) {
      if (options.output_readinfo) {
	readfile_ << locus.chrom << "\t" 
		  << locus.start << "\t" 
		  << locus.end << "\t"
		  << iter->first << "\t" 
		  << "BOUND" << "\t" 
		  << iter->second.data_value - 1 << "\t" 
		  << iter->second.found_pair << std::endl;
      }
      likelihood_maximizer->AddFlankingData(iter->second.data_value - 1); // -1 because flanking is always picked up +1
      flank++;
    } else if (iter->second.read_type == RC_OFFT){
      if (options.output_readinfo) {
	readfile_ << locus.chrom << "\t" 
		  << locus.start << "\t" 
		  << locus.end << "\t"
		  << iter->first << "\t" 
		  << "OFFT" << "\t" 
		  << iter->second.data_value << "\t" 
		  << iter->second.found_pair << std::endl;
      }
      likelihood_maximizer->AddOffTargetData(iter->second.data_value);
      offt++;
    } else {
      continue;
    }
    
  }
  //cerr << span << " " << encl << " " << frr << " " << flank << " " << offt << endl;
  return true;
}

/*
  Main function to decide what to do with each read pair
 */
bool ReadExtractor::ProcessReadPairs(BamCramMultiReader* bamreader,
             const Locus& locus, const int32_t& regionsize, const int32_t& min_match,
             std::map<std::string, ReadPair>* read_pairs) {
  if (locus.end < locus.start){
    // TODO print error "Not enough extracted reads"
    PrintMessageDieOnError("\tLocus end preceeds locus start. Aborting..", M_PROGRESS);
    return false;
  }
  // Get bam alignments from the relevant region
  bamreader->SetRegion(locus.chrom, 
		       locus.start-regionsize, 
		       locus.end+regionsize);

  // Keep track of which file we're processing
  int32_t file_index = 0;
  std::string file_label = "0_";
  std::string prev_file = "";

  // Header has info about chromosome names
  const BamHeader* bam_header = bamreader->bam_header();
  const int32_t chrom_ref_id = bam_header->ref_id(locus.chrom);

  // Go through each alignment in the region
  BamAlignment alignment;

  while (bamreader->GetNextAlignment(alignment)) {
    if (debug) {
      std::cerr << "Processing " << alignment.Name() << std::endl;
    }
    
    // Check if we should skip this read
    if (alignment.IsSupplementary() || alignment.IsSecondary()) {
      continue;
    }


    // Check if we've moved to a different file
    if (prev_file.compare(alignment.Filename()) != 0) {
      prev_file = alignment.Filename();
      std::stringstream ss;
      ss << ++file_index << "_";
      file_label = ss.str();
    }

    // Set key to keep track of this mate pair
    std::string aln_key = file_label + trim_alignment_name(alignment);

    /*  Check if read's mate already processed */
    std::map<std::string, ReadPair>::iterator rp_iter = read_pairs->find(aln_key);
    if (rp_iter != read_pairs->end()) {
      if (debug) {
        std::cerr << "Already found mate  " << alignment.Name()
            << " " << rp_iter->second.read_type
            << std::endl;
      }

      rp_iter->second.found_pair = true;
      rp_iter->second.read2 = alignment;
      // We will check the mate in any case (not just UNKONWN) 
      // (to find longer flanking reads or potential FRRs)
      if (rp_iter->second.read_type != RC_FRR && rp_iter->second.read_type != RC_ENCL){ 
      // if (rp_iter->second.read_type == RC_UNKNOWN) {
        if (debug) {
          std::cerr << "Checking mate " << alignment.Name() 
		    << " " << alignment.QueryBases() << std:: endl;
        }

        int32_t insert_size;
        // This check is probably redundant (SPAN will be found in first round)
        if (rp_iter->second.read_type != RC_SPAN){
          if (FindSpanningRead(alignment, chrom_ref_id, locus, &insert_size)) {
            rp_iter->second.read_type = RC_SPAN;
            rp_iter->second.data_value = insert_size;
            continue;
          }
        }

        int32_t data_value, score_value;
        int32_t nCopy_value = 0;
        ReadType read_type;
        SingleReadType srt;

        ProcessSingleRead(alignment, chrom_ref_id, locus, min_match,
              &data_value, &nCopy_value, &score_value, &read_type, &srt);
	

        if (debug) {
          std::cerr << "Mate found to be   " << read_type << std:: endl;
        }
        if (read_type == RC_FRR || srt == SR_IRR){ // if new guess is FRR
          rp_iter->second.read_type = RC_FRR;
          rp_iter->second.data_value = data_value;

          if (rp_iter->second.max_nCopy < nCopy_value){
            rp_iter->second.max_nCopy = nCopy_value;
          }
        }
        else if ((rp_iter->second.read_type == RC_BOUND) && (read_type == RC_BOUND || 
                  srt == SR_PREFLANK ||
                  srt == SR_POSTFLANK)){  // if new guess is BOUND, just check to update ncopy
          if (rp_iter->second.max_nCopy < nCopy_value){
            rp_iter->second.max_nCopy = nCopy_value;
            rp_iter->second.data_value = nCopy_value;
          }
        }
        else if (read_type == RC_ENCL ||  // If new guess is ENCL, update to ENCL
                   srt == SR_ENCLOSING){
          rp_iter->second.read_type = RC_ENCL;
          rp_iter->second.data_value = data_value;
        }
        else if (rp_iter->second.read_type != RC_UNKNOWN &&
                    read_type == RC_UNKNOWN) // If new guess is UNK and old guess is not, do nothing
        {

        }
        else if (read_type == RC_SPAN) { // If new guess is SPAN
          rp_iter->second.read_type = RC_SPAN;
          rp_iter->second.data_value = data_value;
        }

        // For all cases, update nCopy
        if (rp_iter->second.max_nCopy < nCopy_value){
            rp_iter->second.max_nCopy = nCopy_value;
        }

      }

      continue; // move on to next read
    }
  
    /* Discard read pair if position is irrelevant */
    if (debug) {
      std::cerr << "Checking for discard" << std::endl;
    }
    
    if (FindDiscardedRead(alignment, chrom_ref_id, locus)) {
      ReadPair read_pair;
      read_pair.read_type = RC_DISCARD;
      read_pair.read1 = alignment;
      read_pairs->insert(std::pair<std::string, ReadPair>(aln_key, read_pair));
      continue;
    }



    /* Check if read is spanning */
    if (debug) {
      std::cerr << "Checking for spanning" << std::endl;
    }
    int32_t insert_size;
    if (FindSpanningRead(alignment, chrom_ref_id, locus, &insert_size)) {
      ReadPair read_pair;
      read_pair.read_type = RC_SPAN;
      read_pair.read1 = alignment;
      read_pair.data_value = insert_size;
      read_pairs->insert(std::pair<std::string, ReadPair>(aln_key, read_pair));
      continue;
    }
    //firstpass

    /* If read is at all close to the STR,
       perform realignment to determine read type 
       This will find some enclosing, FRRs, and spanning
    */
    if (debug) {
      std::cerr << "Processing single read" << std::endl;
    }
    
    int32_t data_value, score_value;
    int32_t nCopy_value = 0;
    ReadType read_type;
    ReadPair read_pair;
    SingleReadType srt;
    ProcessSingleRead(alignment, chrom_ref_id, locus, min_match,
          &data_value, &nCopy_value, &score_value, &read_type, &srt);

    read_pair.read_type = read_type;
    read_pair.read1 = alignment;
    read_pair.data_value = data_value;
    read_pair.max_nCopy = nCopy_value;
    read_pairs->insert(std::pair<std::string, ReadPair>(aln_key, read_pair));
  }
  /*  Second pass through reads where only one end processed */
  int32_t num_rescue = 0;
  for (std::map<std::string, ReadPair>::iterator iter = read_pairs->begin();
       iter != read_pairs->end(); iter++) {
    if (iter->second.found_pair || iter->second.read_type == RC_FRR || 
	iter->second.read_type == RC_ENCL || 
	iter->second.read_type == RC_DISCARD) {  
      continue;
    }
    num_rescue++;
    if (num_rescue > 200){
      continue;
    }

    if (debug) {
      std::cerr << "Attempting to rescue mate " << iter->first << std::endl;
    }
    BamAlignment matepair;
    if (!RescueMate(bamreader, iter->second.read1, &matepair)) {
      continue;
    }
    if (debug) {
      std::cerr << "Found mate for " << iter->first << std::endl;
    }

    iter->second.read2 = matepair;

    iter->second.found_pair = true;
    int32_t data_value, score_value;
    int32_t nCopy_value = 0;
    ReadType read_type;
    SingleReadType srt;
    ProcessSingleRead(matepair, chrom_ref_id, locus, min_match,
          &data_value, &nCopy_value, &score_value, &read_type, &srt);

    int32_t read_length = (int32_t)matepair.QueryBases().size();
    if (debug) {
      std::cerr << "Processed mate, found " << read_type << " " << data_value << std::endl;
    }
     
    // We need to check srt, because rescued reads will be classified as RC_UNKNOWN
    // ^^reason: They originate from a different chrom that ProcessSingleRead cannot deal with
     if ((read_type == RC_FRR || srt == SR_IRR || srt == SR_UM_POT_IRR)          // if new guess is FRR
            && nCopy_value >= read_length / locus.period - 1  // and there are enough copies present
            && score_value >= 0.8 * MATCH_SCORE * read_length){ // and the score is high enough TODO set threshold
     
      iter->second.read_type = RC_FRR;
      int32_t data;
      if (iter->second.read1.Position() < locus.start) {
        data = locus.start - (iter->second.read1.Position()+read_length);
      } else {
        data = iter->second.read1.Position() - locus.end;
      }

      iter->second.data_value = data;
      if (iter->second.max_nCopy < nCopy_value){
        iter->second.max_nCopy = nCopy_value;
      }
    }
    else if (read_type == RC_BOUND      // if new guess is BOUND, just check to update ncopy
      || srt == SR_PREFLANK || srt == SR_POSTFLANK
      && nCopy_value >= 1  // and there are enough copies present
            && score_value >= 0.8 * MATCH_SCORE * read_length){ // and the score is high enough TODO set threshold  
      if (iter->second.read_type == RC_UNKNOWN){
        iter->second.read_type = RC_BOUND;
      }
      if (iter->second.max_nCopy < nCopy_value){
        iter->second.max_nCopy = nCopy_value;
        iter->second.data_value = nCopy_value;
      }
    }
    else if (read_type == RC_SPAN ||
          (srt == SR_PREFLANK && alignment.Position() >= max(locus.end - read_length, locus.start)) || 
          (srt == SR_POSTFLANK && alignment.Position() <= max(locus.end - read_length, locus.start))){
      iter->second.read_type = RC_SPAN;
      if (srt == SR_PREFLANK){
        iter->second.data_value = abs(locus.start + nCopy_value*(int32_t)locus.motif.size() - read_length
              - (alignment.Position() + read_length));
      } else if (srt == SR_POSTFLANK){
        iter->second.data_value = abs(locus.end - nCopy_value*(int32_t)locus.motif.size()
              - alignment.Position() + read_length);
      }
      if (iter->second.max_nCopy < nCopy_value){
        iter->second.max_nCopy = nCopy_value;
      }
    }
    else{
      iter->second.read_type = read_type;
      iter->second.data_value = data_value;
      if (iter->second.max_nCopy < nCopy_value){
        iter->second.max_nCopy = nCopy_value;
      }
    }

  }

  
  // Get bam alignments from off target region
  if (locus.offtarget_set){
    for (std::vector<GenomeRegion>::const_iterator reg_it = locus.offtarget_regions.begin();
	 reg_it != locus.offtarget_regions.end(); reg_it++){
      if (options.verbose) {
	stringstream ss;
	ss << "\tAnalyzing off target region: " << reg_it->chrom << ':'
	   << reg_it->start <<  '-' <<  reg_it->end;
	  PrintMessageDieOnError(ss.str(), M_PROGRESS);
      }
      bamreader->SetRegion(reg_it->chrom, reg_it->start, reg_it->end);

      const int32_t offchrom_ref_id = bam_header->ref_id(reg_it->chrom);

      // Go through each alignment in the region
      while (bamreader->GetNextAlignment(alignment)) {
	if (alignment.IsSecondary() or alignment.IsSupplementary())
	  continue;
	// Set key to keep track of this mate pair
	std::string aln_key = file_label + trim_alignment_name(alignment);
	int32_t read_length = (int32_t)alignment.QueryBases().size();
	int32_t data_value, score_value;
	int32_t nCopy_value = 0;
	ReadType read_type;
	ReadPair read_pair;
	SingleReadType srt;
	ProcessSingleRead(alignment, chrom_ref_id, locus, min_match,
			  &data_value, &nCopy_value, &score_value, &read_type, &srt);
       
	//  Check if read's mate already processed
	std::map<std::string, ReadPair>::iterator rp_iter = read_pairs->find(aln_key);
	if (rp_iter->second.found_pair){
	  continue;
	}
	if (rp_iter != read_pairs->end()) {
	  // do another round of rescue maybe?! Currently, naive rescue:
	  rp_iter->second.read2 = alignment;
	  if (read_type == RC_FRR or srt == SR_UM_POT_IRR or srt == SR_IRR){
	    if (rp_iter->second.read_type == RC_POT_OFFT){
	      //cerr << "2\t" << locus.chrom << " " << alignment.QueryBases() << endl;
	      rp_iter->second.read_type = RC_OFFT;
	      rp_iter->second.data_value = -read_length;
	    }
	    else{
	      rp_iter->second.read_type = RC_FRR;
	      rp_iter->second.data_value = data_value;
	    }
	  }
	  else{
	    if (rp_iter->second.read_type == RC_UNKNOWN){
	      rp_iter->second.read_type = read_type;
	      rp_iter->second.data_value = data_value;
	    }
	  }
	}
	else{
	  if (read_type == RC_FRR or srt == SR_UM_POT_IRR){
	    //cerr << "1\t" << locus.chrom << " " << alignment.Name() << endl;
	    read_pair.read_type = RC_POT_OFFT;
	    read_pair.read1 = alignment;
	    read_pair.data_value = -read_length;
	    read_pairs->insert(std::pair<std::string, ReadPair>(aln_key, read_pair));
	  }
	}
    }
  }    
  }  
  
  return true;
}

/*
  Check if read should be discarded
  Discard reads if both mates fall on same
  side of the STR.
  See 5.2_filter_spanning_only_core.py second case
  Return true if yes
*/
bool ReadExtractor::FindDiscardedRead(BamAlignment alignment,
              const int32_t& chrom_ref_id,
              const Locus& locus) {
  // Get read length
  int32_t read_length = options.read_len;
  
  // 5.2_filter_spanning_only_core.py 
  bool discard1 = alignment.RefID() == chrom_ref_id && alignment.Position() <= locus.start-read_length &&
    alignment.MateRefID() == chrom_ref_id && alignment.MatePosition() <= locus.start-read_length;
  bool discard2 = alignment.RefID() == chrom_ref_id && alignment.Position() >= locus.end &&
    alignment.MateRefID() == chrom_ref_id && alignment.MatePosition() >= locus.end;
  // Keep reads that both mates mapped to the same place (sign of unmapped mate)
  bool keep = alignment.RefID() == alignment.MateRefID() && alignment.Position() == alignment.MatePosition();
  return !keep && (discard1 || discard2);
}

/*
  Check if read is spanning
   See 5.2_filter_spanning_only_core.py
   Return true if yes
*/
bool ReadExtractor::FindSpanningRead(BamAlignment alignment,
             const int32_t& chrom_ref_id,
             const Locus& locus,
             int32_t* insert_size) {
  // Get read length info
  int32_t read_length = (int32_t)alignment.QueryBases().size();
  // Similar to 5.2_filter_spanning_only_core.py:57
  // Only includes obvious cases, pre/post flank taken care of elsewhere
  bool span1 = !alignment.StartsWithSoftClip() && alignment.RefID() == chrom_ref_id && alignment.GetEndPosition() <= locus.start &&
    alignment.MateRefID() == chrom_ref_id && alignment.MatePosition() >= locus.end - locus.period; // Margin of size period to find reads that are post flaknking by 1-2 base pairs (may not be found with realigner)
  bool span2 = alignment.RefID() == chrom_ref_id && alignment.Position() >= locus.end &&
    alignment.MateRefID() == chrom_ref_id && alignment.MatePosition() <= locus.start-read_length + locus.period; // Margin of size period to find reads that are pre flaknking by 1-2 base pairs (may not be found with realigner) 
  if (span1 || span2) {
    if ((abs(alignment.TemplateLength()) < 
	 .5 * (abs(alignment.Position() - alignment.MatePosition()) + alignment.Length())) ||
	(abs(alignment.TemplateLength()) > 
	 1.5 * (abs(alignment.Position() - alignment.MatePosition()) + alignment.Length()))){
      *insert_size = abs(alignment.Position() - alignment.MatePosition()) + alignment.Length();
    }
    else{
      *insert_size = abs(alignment.TemplateLength());
    }
    return true;
  }
  return false;
}

/*
  Process a single read using realignment
  
  Return false if something goes wrong
 */
bool ReadExtractor::ProcessSingleRead(BamAlignment alignment,
              const int32_t& chrom_ref_id,
              const Locus& locus,
              const int32_t &min_match,
              int32_t* data_value,
              int32_t* nCopy_value,
              int32_t* score_value,
              ReadType* read_type,
              SingleReadType* srt) {
  *srt = SR_UNKNOWN;

  /* If mapped read in vicinity but not close to STR, save for later */
  if (alignment.IsMapped() && alignment.IsMateMapped() &&
      alignment.RefID() == chrom_ref_id &&
      (alignment.Position() > locus.end || alignment.GetEndPosition() < locus.start) &&
      (alignment.MateRefID() == chrom_ref_id &&
      (alignment.MatePosition() > locus.end + 2 * options.dist_mean || 
       alignment.MatePosition() < locus.start - 2 * options.dist_mean))) {
    *read_type = RC_UNKNOWN;
    *score_value = 0;
    return true;
  }

	
  int32_t start_pos, start_pos_rev, pos_frr, end_frr, score_frr, mismatches_frr;
  int32_t end_pos, end_pos_rev;
  int32_t score, score_rev;
  int32_t nCopy, nCopy_rev;
  FlankMatchState fm_start, fm_end, fm_start_rev, fm_end_rev;
  std::string seq = lowercase(alignment.QueryBases());
  std::string seq_rev = reverse_complement(seq);
  std::string qual = alignment.Qualities();
  int32_t read_length = (int32_t)seq.size();
  //cerr << alignment.RefID() << ":" << alignment.Position()
  //     <<"\tMapped:" << alignment.IsMapped()
  //     <<"\tRev: "<<alignment.IsReverseStrand() 
  //     << "\n" << alignment.QueryBases() << "\n"

  /* Perform realignment and classification */
  if (!expansion_aware_realign(seq, qual, locus.pre_flank, locus.post_flank, locus.motif, min_match,
			       &nCopy, &start_pos, &end_pos, &score, &fm_start, &fm_end)) {
    return false;
  }
  if (!expansion_aware_realign(seq_rev, qual, locus.pre_flank, locus.post_flank, locus.motif, min_match,
			       &nCopy_rev, &start_pos_rev, &end_pos_rev, &score_rev, &fm_start_rev, &fm_end_rev)) {
    return false;
  }

  if (score_rev > score) {
    nCopy = nCopy_rev;
    start_pos = start_pos_rev;
    score = score_rev;
    seq = seq_rev;
    end_pos = end_pos_rev;
    fm_start = fm_start_rev;
    fm_end = fm_end_rev;
  }
  *nCopy_value = nCopy;
  *score_value = score;
  if (alignment.Name() == "CompMultiLoc_7_cov70_readLen_150_ref_hg38_390_altAllele_4946_5457_0:0:0_2:0:0_30a")
    cerr << "In Process read pair:\nsrt:\t\t" << *srt 
	 <<"\n\tQuery Bases\t\t" << alignment.QueryBases()
	 <<"\n\tIsMapped\t\t"<< alignment.IsMapped() 
	 <<"\n\tnCopy, score\t" << nCopy << ", " << score
	 <<"\n\tseq_rev\t" << seq_rev
	 <<"\n\tread type\t\t"<< *read_type<< endl;
  
  if (!classify_realigned_read(seq, locus.motif, 
			       start_pos, end_pos, nCopy, score,
			       (int32_t)locus.pre_flank.size(), 
			       min_match, alignment.IsMapped(), 
			       locus.pre_flank, locus.post_flank, 
			       fm_start, fm_end, srt)) {
    return false;
  }

  if (*srt == SR_UNKNOWN){
    *nCopy_value = 0;
  }  
  
  
  if ((*srt == SR_UNKNOWN || *srt == SR_UM_POT_IRR || *srt == SR_IRR) && 
      alignment.IsMateMapped() &&
      alignment.MatePosition() < locus.end + 2 * options.dist_mean && 
      alignment.MatePosition() > locus.start - 2 * options.dist_mean){
    std::stringstream var_realign_frr;
    for (int i = 0; i<(seq.size() / locus.motif.size() + 1); i++) {
      var_realign_frr << locus.motif;
    }
    std::string frr_ref = var_realign_frr.str();
    striped_smith_waterman(frr_ref, seq, qual, &pos_frr, &end_frr, &score_frr, &mismatches_frr);
    int gaps_frr = abs(int(end_frr - pos_frr - seq.size()));
    // cerr << mismatches_frr << "\t" << seq << endl;
    
    // Tune 0.75 for false positive rate
    if (score_frr > 0.75 * seq.size() * SSW_MATCH_SCORE && 
	mismatches_frr < int(.05 * seq.size()) && 
	gaps_frr < int(.05 * seq.size())){
      // cerr << seq << endl;
      // cerr << mismatches_frr << "\t" << score_frr << endl;
      *srt = SR_IRR;
    }
    else{
      *srt = SR_UNKNOWN;
    }
  }

  if (alignment.IsMapped() == false && alignment.MatePosition() < locus.end + options.dist_mean && alignment.MatePosition() > locus.start - options.dist_mean){
    readfile_ << locus.chrom << "\t" << alignment.Position() << "\t" << alignment.MatePosition() << "\t"
        << alignment.Name() << "\t" << "UNMAPPED" << std::endl << alignment.QueryBases()<<std::endl;
  }

  /*
  // For Unmapped potential IRRs, check if mate is mapped in vicinity of STR locus
  if (*srt == SR_UM_POT_IRR){
    if (alignment.IsMateMapped() &&  
      alignment.MatePosition() < locus.end + (options.dist_mean - options.read_len) && 
      alignment.MatePosition() > locus.start - (options.dist_mean - options.read_len)){
      // cerr << "\nFRR\n"<< alignment.MatePosition() << endl;
      *srt = SR_IRR;
      // cerr << "Seq:   " << alignment.QueryBases() << endl;
      // cerr<< "Score: "<< score << "\tnCopy: " << nCopy << endl;
      // cerr << "BaseQ: " << alignment.Qualities() << endl;
    }
  }
  */

  // Set as UNKNOWN if doesn't pass the score threshold.
  if (score < options.min_score / 100.0 * double(SSW_MATCH_SCORE * options.read_len)){
    *nCopy_value = 0;
    *data_value = 0;
    *srt = SR_UNKNOWN;
    *read_type = RC_UNKNOWN;
    return true;
  }
  
  if (debug) {
    std::cerr << "Processing single read found " << score << " " << start_pos << " " << srt << std::endl;
  }
  // Process according to guessed read type
  /* Spanning cases */
  // 5.2_filter_spanning_only_core.py#L86 - preflank case
  if ((alignment.MateRefID() == chrom_ref_id) &&
      (alignment.MatePosition() >= locus.end - read_length + locus.period) && // Added Margin
      (alignment.MatePosition() >= locus.start + locus.period) &&  // Added this line to deal with pairs with 2 preflanks
      (*srt == SR_PREFLANK)) {
    *read_type = RC_SPAN;
    *data_value = abs(locus.start + nCopy*(int32_t)locus.motif.size() - read_length
          - (alignment.MatePosition() + read_length));
    // if (*data_value > 2000){    // TODO change 2000 to a value based on parameters
    //   *data_value = 0;
    //   *read_type = RC_UNKNOWN;
    // }
    return true;
  }
  // 5.2_filter_spanning_only_core.py#L92 - postflank case
  if ((alignment.MateRefID() == chrom_ref_id) &&
      (alignment.MatePosition() <= locus.start - locus.period) &&  // Added Margin
      (alignment.MatePosition() + read_length <= locus.end - locus.period) && //Added to match preflank checks
      (*srt == SR_POSTFLANK)) {
    *read_type = RC_SPAN;
    *data_value = abs(locus.end - nCopy*(int32_t)locus.motif.size()
          - alignment.MatePosition() + read_length);
    // if (*data_value > 2000){    // TODO change 2000 to a value based on parameters
    //   *data_value = 0;
    //   *read_type = RC_UNKNOWN;
    // }
    return true;
  }
  /* FRR cases */
  // 5.2_filter_FRR_only_core.py:136
  // Added this line to fix issues with mates being mapped to different chroms
  if (*srt == SR_IRR) {
    if (alignment.MateRefID() != chrom_ref_id){
      *read_type = RC_FRR;
      *data_value = -read_length;
      return true;
    }
    *read_type = RC_FRR;
    if (alignment.MatePosition() < locus.start) {
      *data_value = locus.start - (alignment.MatePosition()+read_length);
    } else {
      *data_value = alignment.MatePosition() - locus.end;
    }
    return true;
  }
  /* Enclosing cases */
  if (*srt == SR_ENCLOSING) {
    *read_type = RC_ENCL;
    *data_value = nCopy;
    *nCopy_value = 0;     // nCopy_value is used for flanking heuristic, which is not relevant here
    return true;
  }
  /* Partially spanning cases - get bound */
  if (*srt == SR_PREFLANK || *srt == SR_POSTFLANK) {
    *read_type = RC_BOUND;
    *data_value = nCopy;
    return true;
  }
  /* If we get here, we're not sure */
  *read_type = RC_UNKNOWN;
  return true;
}

/*
  Rescue mate if the mate didn't get mapped to the right region
  TODO: this doesn't handle if the mate is unaligned?
 */

bool ReadExtractor::RescueMate(BamCramMultiReader* bamreader,
             BamAlignment alignment, BamAlignment* matepair) {
  const BamHeader* bam_header = bamreader->bam_header();
  std::string aln_key1 = trim_alignment_name(alignment);
  if (debug) {
    std::cerr << "Looking for mate in " << bam_header->ref_name(alignment.MateRefID()) <<
      " " << alignment.MatePosition() << std::endl;
  }

  bamreader->SetRegion(bam_header->ref_name(alignment.MateRefID()),
           alignment.MatePosition()-1, alignment.MatePosition()+1);
  BamAlignment aln;
  int32_t count = 0;
  while (bamreader->GetNextAlignment(aln)) {
    std::string aln_key2 = trim_alignment_name(aln);
    count++;
    if (count > 50){ // Skip this region if mate was not found in the first 50 alignments
      return false;
    }
    if (debug) {
      std::cerr << "Looking for " << aln_key1 << " found " << aln_key2 << std::endl;
    }
    if (aln_key1 == aln_key2) {
      *matepair = aln;
      return true;
    }
  }
  return false;
}

std::string ReadExtractor::trim_alignment_name(const BamAlignment& aln) const {
  std::string aln_name = aln.Name();
  if (aln_name.size() > 2){
    if (aln_name[aln_name.size()-2] == '/')
      aln_name.resize(aln_name.size()-2);
  }
  return aln_name;
}

// Implemented in BamInfoExtract. TODO delete
// /*
//   Computing the insert size distribution
//  */
// bool ReadExtractor::ComputeInsertSizeDistribution(BamCramMultiReader* bamreader,
//              const Locus& locus,
//              double* mean, double* std_dev, int32_t* read_len) {
//   // TODO change 200000 flank size to something appropriate
//   int32_t flank_size = 200000;
//   int32_t exclusion_margin = 1000;

//   double mean_b, mean_a, std_b, std_a; // mean and std dev, before and after locus
  
//   // Get bam alignments from the relevant region
//   // bamreader->SetRegion(locus.chrom, locus.start-flank_size>0?locus.start-flank_size:0, locus.end+flank_size);


//   // Header has info about chromosome names
//   const BamHeader* bam_header = bamreader->bam_header();
//   const int32_t chrom_ref_id = bam_header->ref_id(locus.chrom);

//   int32_t median, size = 0, sum = 0, valid_size = 0, sum_std = 0;
//   std::vector<int32_t> temp_len_vec, valid_temp_len_vec;
//   BamAlignment alignment;

//   // collecting reads mapped before locus
//   bamreader->SetRegion(locus.chrom, 
//       locus.start - flank_size > 0 ? locus.start - flank_size : 0, 
//       locus.start - exclusion_margin > 0 ? locus.start - exclusion_margin : 0);

//   // Go through each alignment in the region
//   while (bamreader->GetNextAlignment(alignment)) {
//     // Set template length
//     temp_len_vec.push_back(abs(alignment.TemplateLength()));
//     size++;
//   }
//   *read_len = (int32_t)(alignment.QueryBases().size());
//   // collecting reads mapped after locus
//   bamreader->SetRegion(locus.chrom, 
//       locus.start + exclusion_margin, 
//       locus.start + flank_size);
//   // Go through each alignment in the region
//   while (bamreader->GetNextAlignment(alignment)) {
//     // Set template length
//     temp_len_vec.push_back(abs(alignment.TemplateLength()));
//     size++;
//   }

//   // Give up if we don't have enough reads TODO probably need more than 0...
//   if (temp_len_vec.size() == 0) {
//     return false;
//   }

//   sort(temp_len_vec.begin(), temp_len_vec.end());
//   median = temp_len_vec.at(int32_t(size / 2));
  
//   for (std::vector<int32_t>::iterator temp_it = temp_len_vec.begin();
//        temp_it != temp_len_vec.end();
//        ++temp_it) {
//     // Todo change 3
//     if(*temp_it < 3 * median){
//       valid_temp_len_vec.push_back(*temp_it);
//       valid_size++;  
//     }
//   }
//   int* valid_temp_len_arr = &valid_temp_len_vec[0];
//   *mean = gsl_stats_int_mean(valid_temp_len_arr, 1, valid_size - 1);
//   *std_dev = gsl_stats_int_sd_m (valid_temp_len_arr,  1, valid_size, *mean);
//   dist_mean = *mean;
//   dist_sdev = *std_dev;
//   return true;  //TODO add false case
// }

ReadExtractor::~ReadExtractor() {
  if (options.output_readinfo) {
    readfile_.close();
  }
}

