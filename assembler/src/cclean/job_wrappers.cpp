#include <set>
#include "job_wrappers.hpp"
#include "logger/log_writers.hpp"

double is_alignment_good(const StripedSmithWaterman::Alignment& a, const std::string& query, int aligned_part_fraction) {
	return (std::min(a.query_end, a.ref_end) - std::max(a.query_begin, a.ref_begin)) / (double) query.size() > aligned_part_fraction;
}

bool AlignmentJobWrapper::operator()(const Read &r) {
	try {
		const std::string &name = r.getName();
		const std::string &ref = r.getSequenceString();
		auto it = data->get_data_iterator();
		for (unsigned i = 0; i < data->get_sequences_amount(); ++i) {
			StripedSmithWaterman::Aligner aligner;
			StripedSmithWaterman::Filter filter;
			StripedSmithWaterman::Alignment alignment;
			const std::string &query = *(it->second);
			aligner.Align(query.c_str(), ref.c_str(), ref.size(), filter, &alignment);
			std::string& database_name = *(it->first);

			if (alignment.mismatches < mismatch_threshold && is_alignment_good(alignment, query, aligned_part_fraction)) {
#       pragma omp critical
        {
          print_alignment(output, alignment, ref, query, name, database_name);
          print_bed(bed, name, alignment.ref_begin, alignment.ref_end);
        }
			}
			it++;
		}
	} catch (std::exception& e) {
		ERROR(e.what());
	}
	return false;
}

bool ExactMatchJobWrapper::operator()(const Read &r) {
	try {
		const std::string& name = r.getName();
		const std::string& sequence = r.getSequenceString();
		seq2index_t res = ahoCorasick.search(sequence);

		if (res.size() > 0) {
#     pragma omp critical
      {
        print_match(output, bed, res, name, sequence, data);
      }
		}
	} catch (std::exception& e) {
		ERROR(e.what());
	}
	return false;
}

bool ExactAndAlignJobWrapper::operator()(const Read &r) {
	try{
		const std::string& name = r.getName();
		const std::string& sequence = r.getSequenceString();

		//try to exact match the sequences
		auto matchingSequences = dbAhoCorasick.search(sequence);
		if (!matchingSequences.empty()) {
#pragma omp critical
			print_match(output, bed, matchingSequences, name, sequence, data);
			return false; //exact match is better than an alignment -> no need to align
		}

		//try to search in kmers db
		auto matchingKmers = kmersAhoCorasick.search(sequence);
		std::set<std::string * , Compare> setOfContaminations2check;
		for (auto it = matchingKmers.begin(); it != matchingKmers.end(); ++it) {
			std::set<std::string *, Compare> setOfSeqs;
			data->get_sequences_for_kmer(*(it->first), setOfSeqs);
			setOfContaminations2check.insert(setOfSeqs.begin(), setOfSeqs.end());
		}

		//try to align the contaminations for corresponding kmers
		for (auto it = setOfContaminations2check.begin(); it != setOfContaminations2check.end(); ++it) {
			StripedSmithWaterman::Aligner aligner;
			StripedSmithWaterman::Filter filter;
			StripedSmithWaterman::Alignment alignment;
			std::string& query = *(*it);
			aligner.Align(query.c_str(), sequence.c_str(), sequence.size(), filter, &alignment);

			std::string database_name;
			data->get_name_by_sequence(query, database_name);

#pragma omp critical
			if (alignment.mismatches < mismatch_threshold && is_alignment_good(alignment, query, aligned_part_fraction)) {
				print_alignment(output, alignment, sequence, query, name, database_name);
				print_bed(bed, name, alignment.ref_begin, alignment.ref_end);
			}
		}

	} catch (std::exception& e) {
		ERROR(e.what());
	}
	return false;
}
