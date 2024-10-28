#define FRU__6BIT_MAXCHAR ((1 << 6) - 1)
bool fru__validate_6bit(const char * string)
{
	size_t i;

	for (i = 0; string[i]; ++i) {
		char c = string[i] - ' '; // Space is zero
		if (c > FRU__6BIT_MAXFULLLENGTH) {
			fru_errno = FERANGE;
			return false;
		}
	}

	if (FRU__6BIT_LENGTH(i) > FRU__FIELDMAXLEN) {
			fru_errno = FELONGINPUT;
			return false;
	}

	return true;
}
