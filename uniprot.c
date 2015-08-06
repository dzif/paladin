#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "uniprot.h"
#include "protein.h"

UniprotList * uniprotPriEntryLists = 0;
UniprotList * uniprotSecEntryLists = 0;
int uniprotPriListCount = 0;
int uniprotSecListCount = 0;

const char * downloadNames[] = {"uniprot_sprot.fasta.gz", 
			  "uniprot_trembl.fasta.gz"};
const char * downloadURLs[] = {"ftp://ftp.uniprot.org/pub/databases/uniprot/current_release/knowledgebase/complete/uniprot_sprot.fasta.gz",
			 "ftp://ftp.uniprot.org/pub/databases/uniprot/current_release/knowledgebase/complete/uniprot_trembl.fasta.gz"};

UniprotList * getGlobalLists(int passPrimary) {
	if (passPrimary) return uniprotPriEntryLists;
	else return uniprotSecEntryLists;
}

int * getGlobalCount(int passPrimary) {
	if (passPrimary) return &uniprotPriListCount;
	else return &uniprotSecListCount;
}

void prepareUniprotReport(int passType, int passPrimary, UniprotList * passLists, CURLBuffer * passBuffer) {
	UniprotList * globalLists;

	// Aggregate and sort lists by value
	prepareUniprotLists(passLists, passPrimary);

	// Do not process if no results
	globalLists = getGlobalLists(passPrimary);
	if (globalLists[0].entryCount == 0) return;

	// Report specific preparation
	if (passType == OUTPUT_TYPE_UNIPROT_FULL) {
		// Submit entries to UniProt and retrieve full information
		retrieveUniprotOnline(passLists + UNIPROT_LIST_FULL, passBuffer);
		joinOnlineLists(passLists + UNIPROT_LIST_FULL, passBuffer->buffer);
	}

	// Sort aggregated lists by count
	qsort(passLists[UNIPROT_LIST_FULL].entries, passLists[UNIPROT_LIST_FULL].entryCount, sizeof(UniprotEntry), uniprotEntryCompareID);
	qsort(passLists[UNIPROT_LIST_GENES].entries, passLists[UNIPROT_LIST_GENES].entryCount, sizeof(UniprotEntry), uniprotEntryCompareGene);
	qsort(passLists[UNIPROT_LIST_ORGANISM].entries, passLists[UNIPROT_LIST_ORGANISM].entryCount, sizeof(UniprotEntry), uniprotEntryCompareOrganism);
}

void renderUniprotReport(int passType, int passPrimary, FILE * passStream) {
	UniprotList * globalLists;
	UniprotList uniprotLists[3];
	CURLBuffer tempBuffer;

	// Prepare data
	prepareUniprotReport(passType, passPrimary, uniprotLists, &tempBuffer);

	// Report no data
	globalLists = getGlobalLists(passPrimary);
	if (globalLists[0].entryCount == 0) {
		fprintf(passStream, "No entries to report\n");
		return;
	}

	// Render requested report
	switch (passType) {
		case OUTPUT_TYPE_UNIPROT_SIMPLE:
			fprintf(passStream, "Count\tCount %%\tUniProtKB\n");
			renderUniprotEntries(uniprotLists + UNIPROT_LIST_FULL, UNIPROT_LIST_FULL, passStream);
			fprintf(passStream, "\n\nCount\tCount %%\tGene\n");
			renderUniprotEntries(uniprotLists + UNIPROT_LIST_GENES, UNIPROT_LIST_GENES, passStream);
			fprintf(passStream, "\n\nCount\tCount %%\tOrganism\n");
			renderUniprotEntries(uniprotLists + UNIPROT_LIST_ORGANISM, UNIPROT_LIST_ORGANISM, passStream);

			break;

		case OUTPUT_TYPE_UNIPROT_FULL:
			fprintf(passStream, "Count\tCount %%\tUniProtKB\tID\tOrganism\tProtein Names\tGenes\tPathway\tFeatures\tGene Ontology\tReviewed\tExistence\tComments\n");
			renderUniprotEntries(uniprotLists + UNIPROT_LIST_FULL, UNIPROT_LIST_FULL, passStream);
			freeCURLBuffer(&tempBuffer);

			break;
	}

	cleanUniprotLists(uniprotLists, passPrimary);
}

const char * downloadUniprotReference(int passReference) {
	CURL * curlHandle;
	CURLcode curlResult;
	FILE * fileHandle;
	const char * retFile;

	curlHandle = curl_easy_init();
	fileHandle = fopen(downloadNames[passReference], "w");
	retFile = downloadNames[passReference];

	curl_easy_setopt(curlHandle, CURLOPT_URL, downloadURLs[passReference]);
	curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, fileHandle) ;

	fprintf(stderr, "[M::%s] Downloading %s...\n", __func__, downloadURLs[passReference]);

	curlResult = curl_easy_perform(curlHandle);

	if (curlResult != CURLE_OK) {
		fprintf(stderr, "ERROR: %s\n", curl_easy_strerror(curlResult));
		unlink(downloadNames[passReference]);
		retFile = "";
	}
		
	curl_easy_cleanup(curlHandle);
	fclose(fileHandle);

	return retFile;
}

void retrieveUniprotOnline(UniprotList * passList, CURLBuffer * retBuffer) {
	int entryIdx, queryIdx, parseIdx, errorIdx, queryCount;
	CURL * curlHandle;
	CURLcode curlResult;
	CURLBuffer tempBuffer;
	char queryString[UNIPROT_MAX_SUBMIT * 50], jobID[50];
	char * httpString;

	// Init structures
	curlHandle = curl_easy_init();
	initCURLBuffer(retBuffer, UNIPROT_BUFFER_GROW);
	initCURLBuffer(&tempBuffer, UNIPROT_BUFFER_GROW);
	queryCount = (passList->entryCount < UNIPROT_MAX_SUBMIT) ? passList->entryCount : UNIPROT_MAX_SUBMIT;

	for (entryIdx = 0 ; entryIdx < passList->entryCount ; ) {
		// Build query string
		queryString[0] = 0;
		for (queryIdx = 0 ; (queryIdx < queryCount) && (entryIdx < passList->entryCount) ; entryIdx++) {
			for (parseIdx = 0 ; parseIdx < strlen(passList->entries[entryIdx].id) ; parseIdx++) {
				if (passList->entries[entryIdx].id[parseIdx] == '_') {
					sprintf(queryString, "%s%s ", queryString, passList->entries[entryIdx].id);
					queryIdx++;
					break;
				}
			}
		}

		httpString = curl_easy_escape(curlHandle, queryString, 0);
		sprintf(queryString, "uploadQuery=%s&format=job&from=ACC+ID&to=ACC&landingPage=false", httpString);
		curl_free(httpString);

		if (bwa_verbose >= 3) {
			fprintf(stderr, "[M::%s] Submitted %d of %d entries to UniProt...\n", __func__, entryIdx, passList->entryCount);
		}

		// Restart a limited number of times if errors encountered
		for (errorIdx = 0 ; errorIdx < UNIPROT_MAX_ERROR ; errorIdx++) {
			// Stage 1 - Submit query for processing
			curl_easy_setopt(curlHandle, CURLOPT_URL, "http://www.uniprot.org/uploadlists/");
			curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, queryString);
			curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, receiveUniprotOutput);
			curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &tempBuffer);

			resetCURLBuffer(&tempBuffer);
			curlResult = curl_easy_perform(curlHandle);

			if (curlResult != CURLE_OK) {
				fprintf(stderr, "ERROR: %s\n", curl_easy_strerror(curlResult));
				continue;
			}

			// Stage 2 - Wait for results
			sprintf(jobID, "%s", tempBuffer.buffer);
			sprintf(queryString, "http://www.uniprot.org/jobs/%s.stat", jobID);

			resetCURLBuffer(&tempBuffer);
			curl_easy_setopt(curlHandle, CURLOPT_URL, queryString);

			while (strcmp(tempBuffer.buffer, "COMPLETED") != 0) {
				sleep(1);
				resetCURLBuffer(&tempBuffer);
				curlResult = curl_easy_perform(curlHandle);
				if (curlResult != CURLE_OK) break;
			}

			if (curlResult != CURLE_OK) {
				fprintf(stderr, "ERROR: %s\n", curl_easy_strerror(curlResult));
				continue;
			}

			// Stage 3 - Retrieve Results
			sprintf(queryString, "query=job:%s&format=tab&columns=entry%%20name,id,organism,protein%%20names,genes,pathway,features,go,reviewed,existence,comments", jobID);
			curl_easy_setopt(curlHandle, CURLOPT_URL, "http://www.uniprot.org/uniprot/");
			curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, retBuffer);

			curlResult = curl_easy_perform(curlHandle);

			if (curlResult != CURLE_OK) {
				fprintf(stderr, "ERROR: %s\n", curl_easy_strerror(curlResult));
				continue;
			}

			break;
		}
	}

	curl_easy_cleanup(curlHandle);
	freeCURLBuffer(&tempBuffer);
}

void renderUniprotEntries(UniprotList * passList, int passType, FILE * passStream) {
	int entryIdx, occurTotal;
	float occurPercent;

	// Count total occurrences for percentages
	for (entryIdx = 0, occurTotal = 0 ; entryIdx < passList->entryCount ; entryIdx++) {
		occurTotal += passList->entries[entryIdx].numOccurrence;
	}

	// Render fields
	for (entryIdx = 0 ; entryIdx < passList->entryCount ; entryIdx++) {
		occurPercent = (float) passList->entries[entryIdx].numOccurrence / (float ) occurTotal * 100;
		switch(passType) {
			case UNIPROT_LIST_FULL:
				fprintf(passStream, "%d\t%.2f\t%s\n", passList->entries[entryIdx].numOccurrence, occurPercent, passList->entries[entryIdx].id);
				break;
			case UNIPROT_LIST_GENES:
				fprintf(passStream, "%d\t%.2f\t%s\n", passList->entries[entryIdx].numOccurrence, occurPercent, passList->entries[entryIdx].gene);
				break;
			case UNIPROT_LIST_ORGANISM:
				fprintf(passStream, "%d\t%.2f\t%s\n", passList->entries[entryIdx].numOccurrence, occurPercent, passList->entries[entryIdx].organism);
				break;
		}
	}
}

void renderNumberAligned(const mem_opt_t * passOptions) {
	int listIdx, successTotal, alignTotal;

	// Primary alignments
	for (successTotal = 0, alignTotal = 0, listIdx = 0 ; listIdx < uniprotPriListCount ; listIdx++) {
		successTotal += uniprotPriEntryLists[listIdx].entryCount;
		alignTotal += uniprotPriEntryLists[listIdx].entryCount + uniprotPriEntryLists[listIdx].unalignedCount;
	}

	// Secondary alignments (if requested)
	if (passOptions->flag & MEM_F_ALL) {
		for (listIdx = 0 ; listIdx < uniprotSecListCount ; listIdx++) {
			successTotal += uniprotSecEntryLists[listIdx].entryCount;
			alignTotal += uniprotSecEntryLists[listIdx].entryCount;
		}
	}

	if (alignTotal == 0) {
		fprintf(stderr, "[M::%s] No detected ORFs, no alignment performed\n", __func__);
	}
	else {
		fprintf(stderr, "[M::%s] Aligned %d out of %d total detected ORFs (%.2f%%)\n", __func__, successTotal, alignTotal, (float) successTotal / (float) alignTotal * 100);
	}
}

int addUniprotList(worker_t * passWorker, int passSize, int passFull) {
	int entryIdx, alnIdx, addPriIdx, addSecIdx, parseIdx;
	int refID, alignType, primaryCount, totalAlign;
	UniprotList * globalLists;
	int * globalCount, * currentIdx;
	char * uniprotEntry;

	// Create lists
	uniprotPriEntryLists = realloc(uniprotPriEntryLists, (uniprotPriListCount + 1) * sizeof(UniprotList));
	memset(uniprotPriEntryLists + uniprotPriListCount, 0, sizeof(UniprotList));
	uniprotSecEntryLists = realloc(uniprotSecEntryLists, (uniprotSecListCount + 1) * sizeof(UniprotList));
	memset(uniprotSecEntryLists + uniprotSecListCount, 0, sizeof(UniprotList));

	// Calculate potential total list size
	for (entryIdx = 0, totalAlign = 0 ; entryIdx < passSize ; entryIdx++) {
		// Only add active sequences
		if (!passWorker->regs[entryIdx].active) continue;

		for (alnIdx = 0, primaryCount = 0 ; alnIdx < passWorker->regs[entryIdx].n ; alnIdx++) {
			switch (alignType = getAlignmentType(passWorker, entryIdx, alnIdx)) {
				case MEM_ALIGN_PRIMARY:
					uniprotPriEntryLists[uniprotPriListCount].entryCount++;
					// Count non-linear as total alignments
					if (primaryCount++) totalAlign++;
					break;
				case MEM_ALIGN_SECONDARY:
					uniprotSecEntryLists[uniprotSecListCount].entryCount++; break;
			}
		}	

		totalAlign++;
	}

	// Set counts and allocate entries
	uniprotPriEntryLists[uniprotPriListCount].unalignedCount = totalAlign - uniprotPriEntryLists[uniprotPriListCount].entryCount;
	uniprotSecEntryLists[uniprotSecListCount].unalignedCount = totalAlign - uniprotSecEntryLists[uniprotSecListCount].entryCount;;
	uniprotPriEntryLists[uniprotPriListCount].entries = calloc(uniprotPriEntryLists[uniprotPriListCount].entryCount, sizeof(UniprotEntry));
	uniprotSecEntryLists[uniprotSecListCount].entries = calloc(uniprotSecEntryLists[uniprotSecListCount].entryCount, sizeof(UniprotEntry));

	// Populate list
	for (addPriIdx = 0, addSecIdx = 0, entryIdx = 0 ; entryIdx < passSize && passFull ; entryIdx++) {
		// Only add active sequences
		if (!passWorker->regs[entryIdx].active) continue;

		for (alnIdx = 0 ; alnIdx < passWorker->regs[entryIdx].n ; alnIdx++) {
			// Only add successful alignments
			if ((alignType = getAlignmentType(passWorker, entryIdx, alnIdx)) < MEM_ALIGN_PRIMARY) continue;

			globalLists = getGlobalLists(alignType == MEM_ALIGN_PRIMARY);
			globalCount = getGlobalCount(alignType == MEM_ALIGN_PRIMARY);
			currentIdx = (alignType == MEM_ALIGN_PRIMARY) ? &addPriIdx : &addSecIdx;

			// Extract ID and entry
			refID = passWorker->regs[entryIdx].a[alnIdx].rid;
			uniprotEntry = passWorker->bns->anns[refID].name;

			// Strip sequence and frame info for nucleotide references
			if (passWorker->opt->indexFlag & INDEX_FLAG_NT)
			for (parseIdx = 2 ; (parseIdx > 0) && (*uniprotEntry != 0) ; uniprotEntry++) {
				if (*uniprotEntry == ':') parseIdx--;
			}

			// Strip initial IDs
			for (parseIdx = 2 ; (parseIdx > 0) && (*uniprotEntry != 0) ; uniprotEntry++) {
				if (*uniprotEntry == '|') parseIdx--;
			}

			// Strip description
			for (parseIdx = 0 ; uniprotEntry[parseIdx] != 0 ; parseIdx++) {
				if (uniprotEntry[parseIdx] == ' ') {
					uniprotEntry[parseIdx] = 0;
					break;
				}
			}

			// Full ID
			globalLists[*globalCount].entries[*currentIdx].id = malloc(strlen(uniprotEntry) + 1);
			globalLists[*globalCount].entries[*currentIdx].numOccurrence = 1;
			sprintf(globalLists[*globalCount].entries[*currentIdx].id, "%s", uniprotEntry);

			// Gene/organism
			for (parseIdx = 0 ; parseIdx < strlen(uniprotEntry) ; parseIdx++) {
				if (*(uniprotEntry + parseIdx) == '_') {
					globalLists[*globalCount].entries[*currentIdx].gene = malloc(parseIdx + 1);
					sprintf(globalLists[*globalCount].entries[*currentIdx].gene, "%.*s", parseIdx, uniprotEntry);
					globalLists[*globalCount].entries[*currentIdx].organism = malloc(strlen(uniprotEntry + parseIdx) + 1);
					sprintf(globalLists[*globalCount].entries[*currentIdx].organism, "%s", uniprotEntry + parseIdx + 1);
					break;
				}
			}

			(*currentIdx)++;
		}
	}

	uniprotPriListCount++;
	uniprotSecListCount++;

	return uniprotPriListCount - 1;
}

void cleanUniprotLists(UniprotList * passLists, int passPrimary) {
	UniprotList * globalLists;
	int listIdx, entryIdx;

	globalLists = getGlobalLists(passPrimary);

	// Global list 0 contains pointers to all allocated strings, so delete from there. Do not repeat with other lists
	for (entryIdx = 0 ; entryIdx < globalLists[0].entryCount ; entryIdx++) {
		free(globalLists[0].entries[entryIdx].id);
		free(globalLists[0].entries[entryIdx].gene);
		free(globalLists[0].entries[entryIdx].organism);
	}

	free(globalLists[0].entries);
	free(globalLists);

	// Free local lists
	for (listIdx = 0 ; listIdx < 3 ; listIdx++) {
		free(passLists[listIdx].entries);
	}
}

size_t receiveUniprotOutput(void * passString, size_t passSize, size_t passNum, void * retStream) {
	CURLBuffer * currentBuffer;

	currentBuffer = (CURLBuffer *) retStream;

	// Grow receive buffer if addition is greater than capacity
	if (currentBuffer->size + (int)(passSize * passNum) >= currentBuffer->capacity) {
		currentBuffer->buffer = realloc(currentBuffer->buffer, currentBuffer->capacity + UNIPROT_BUFFER_GROW);
		currentBuffer->capacity += UNIPROT_BUFFER_GROW;
        }

	// Concatenate results
	memcpy(currentBuffer->buffer + currentBuffer->size, passString, (int) (passSize * passNum));
	currentBuffer->size += (int) (passSize * passNum);
	currentBuffer->buffer[currentBuffer->size] = 0;

	return (size_t)(passSize * passNum);
}


void prepareUniprotLists(UniprotList * retLists, int passPrimary) {
	UniprotList * globalLists;
	int listIdx, entryIdx, localIdx;
	int maxEntries, totalSize;

	globalLists = getGlobalLists(passPrimary);

	// Calculate maximum number of UniProt entries
	for (maxEntries = 0, listIdx = 0 ; listIdx < *getGlobalCount(passPrimary) ; listIdx++) {
		maxEntries += globalLists[listIdx].entryCount;
	}

	if (bwa_verbose >= 3) {
		fprintf(stderr, "[M::%s] Aggregating %d entries for UniProt report\n", __func__, maxEntries);
	}

	// Stop processing if no entries, but ensure a list exists for easier post-processing
	if (maxEntries == 0) {
		if (globalLists == NULL) {
			if (passPrimary) uniprotPriEntryLists = malloc(sizeof(UniprotList));
			else uniprotSecEntryLists = malloc(sizeof(UniprotList));

			globalLists = getGlobalLists(passPrimary);
			globalLists[0].entries = NULL;
			globalLists[0].entryCount = 0;
			globalLists[0].unalignedCount = 0;
			*getGlobalCount(passPrimary) = 1;
		}

		return;
	}

	// Join each pipeline's individual list into one master list (at first list)
	for (listIdx = 0, totalSize = 0 ; listIdx < *getGlobalCount(passPrimary) ; listIdx++) {
		totalSize += globalLists[listIdx].entryCount;
	}

	globalLists[0].entries = realloc(globalLists[0].entries, sizeof(UniprotEntry) * totalSize);

	for (listIdx = 1, entryIdx = globalLists[0].entryCount ; listIdx < *getGlobalCount(passPrimary) ; listIdx++) {
		memcpy(globalLists[0].entries + entryIdx, globalLists[listIdx].entries, sizeof(UniprotEntry) * globalLists[listIdx].entryCount);
		entryIdx += globalLists[listIdx].entryCount;
		free(globalLists[listIdx].entries);
		globalLists[listIdx].entryCount = 0;
	}

	globalLists[0].entryCount = totalSize;

	// Aggregate each local list
	for (localIdx = 0 ; localIdx < 3 ; localIdx++) {
		aggregateUniprotList(retLists + localIdx, localIdx, passPrimary);
	}
}

void joinOnlineLists(UniprotList * retList, char * passUniprotOutput) {
	char * * lineIndices;
	int parseIdx, entryIdx, lineIdx;
	int lineCount, outputSize, matchValue;

	// Count number of lines in output
	outputSize = strlen(passUniprotOutput);

	for (lineCount = 0, parseIdx = 0 ; parseIdx < outputSize ; parseIdx++) {
		if (passUniprotOutput[parseIdx] == '\n') lineCount++;
	}
	if (passUniprotOutput[parseIdx - 1] != '\n') lineCount++;

	// Index each line
	lineIndices = malloc(lineCount * sizeof(char *));
	lineIndices[0] = passUniprotOutput;

	for (lineIdx = 1, parseIdx = 0 ; parseIdx < outputSize ; parseIdx++) {
		// If EOL found, change to NULL and check for next valid line
		if (passUniprotOutput[parseIdx] == '\n') {
			passUniprotOutput[parseIdx] = 0;
			if (parseIdx < outputSize - 1) {
				lineIndices[lineIdx++] = passUniprotOutput + parseIdx + 1;
			}
		}
	}

	// Sort (hopefully temporarily)
	qsort(lineIndices, lineCount, sizeof(char *), uniprotEntryCompareOnline);

	// Now cross reference/join - both are ordered, so skip in lexicographical order if match not found
	for (entryIdx = 0, lineIdx = 1 ; (entryIdx < retList->entryCount) && (lineIdx < lineCount)  ; ) {
		matchValue = strncmp(retList->entries[entryIdx].id, lineIndices[lineIdx], strlen(retList->entries[entryIdx].id));

		if (matchValue == 0) {
			// Do not free existing string - global list handles this during cleanup
			retList->entries[entryIdx++].id = lineIndices[lineIdx++];
		}
		else if (matchValue > 0) lineIdx++;
		else entryIdx++; 
	}

	free(lineIndices);
}

void aggregateUniprotList(UniprotList * retList, int passListType, int passPrimary) {
	UniprotList * globalLists;
	int entryIdx;
	int memberOffset;
	char * srcBase, * dstBase;

	globalLists = getGlobalLists(passPrimary);

	// First sort full list
	switch (passListType) {
		case UNIPROT_LIST_FULL:
			qsort(globalLists[0].entries, globalLists[0].entryCount, sizeof(UniprotEntry), uniprotEntryCompareID);
			memberOffset = offsetof(UniprotEntry, id);
			break;
		case UNIPROT_LIST_GENES:
			qsort(globalLists[0].entries, globalLists[0].entryCount, sizeof(UniprotEntry), uniprotEntryCompareGene);
			memberOffset = offsetof(UniprotEntry, gene);
			break;
		case UNIPROT_LIST_ORGANISM:
			qsort(globalLists[0].entries, globalLists[0].entryCount, sizeof(UniprotEntry), uniprotEntryCompareOrganism);
			memberOffset = offsetof(UniprotEntry, organism);
			break;
		default: memberOffset = 0; break;
	}

	// Count entry occurrences and aggregate into return list
	retList->entries = calloc(globalLists[0].entryCount, sizeof(UniprotEntry));
	retList->entries[0].id = globalLists[0].entries[0].id;
	retList->entries[0].gene = globalLists[0].entries[0].gene;
	retList->entries[0].organism = globalLists[0].entries[0].organism;
	retList->entryCount = 0;

	for (entryIdx = 0 ; entryIdx < globalLists[0].entryCount ; entryIdx++) {
		srcBase = (char *) (globalLists[0].entries + entryIdx);
		dstBase = (char *) (retList->entries + retList->entryCount);

		// If current global entry doesn't match local entry, create new local entry and copy pointer to relevant string info
		if (strcmp(*((char * *) (srcBase + memberOffset)), *((char * *) (dstBase + memberOffset))) != 0) {
			dstBase = (char *) (retList->entries + ++retList->entryCount);
			*((char * *) (dstBase + memberOffset)) = *((char * *) (srcBase + memberOffset));
		}

		retList->entries[retList->entryCount].numOccurrence++;
	}

	retList->entryCount++;
}


int uniprotEntryCompareID (const void * passEntry1, const void * passEntry2) {
	int compVal = 0;

	if (((compVal = ((UniprotEntry *)passEntry2)->numOccurrence - ((UniprotEntry *)passEntry1)->numOccurrence)) != 0) {
		return compVal;
	}

	if ((compVal = strcmp(((UniprotEntry *)passEntry1)->id, ((UniprotEntry *)passEntry2)->id)) != 0) {
		return compVal;
	}

	return compVal;
}

int uniprotEntryCompareGene (const void * passEntry1, const void * passEntry2) {
	int compVal = 0;

	if (((compVal = ((UniprotEntry *)passEntry2)->numOccurrence - ((UniprotEntry *)passEntry1)->numOccurrence)) != 0) {
		return compVal;
	}

	if ((compVal = strcmp(((UniprotEntry *)passEntry1)->gene, ((UniprotEntry *)passEntry2)->gene)) != 0) {
		return compVal;
	}

	return compVal;
}

int uniprotEntryCompareOrganism (const void * passEntry1, const void * passEntry2) {
	int compVal = 0;

	if (((compVal = ((UniprotEntry *)passEntry2)->numOccurrence - ((UniprotEntry *)passEntry1)->numOccurrence)) != 0) {
		return compVal;
	}

	if ((compVal = strcmp(((UniprotEntry *)passEntry1)->organism, ((UniprotEntry *)passEntry2)->organism)) != 0) {
		return compVal;
	}

	return compVal;
}

int uniprotEntryCompareOnline (const void * passEntry1, const void * passEntry2) {
	return (strcmp(*((char * *)passEntry1), *((char * *)passEntry2)));
}

void initCURLBuffer(CURLBuffer * passBuffer, int passCapacity) {
	passBuffer->buffer = calloc(1, passCapacity);
	passBuffer->capacity = passCapacity;
	passBuffer->size = 0;
	passBuffer->buffer[0] = 0;
}

void resetCURLBuffer(CURLBuffer * passBuffer) {
	passBuffer->size = 0;
	passBuffer->buffer[0] = 0;
}

void freeCURLBuffer(CURLBuffer * passBuffer) {
	free(passBuffer->buffer);
}
